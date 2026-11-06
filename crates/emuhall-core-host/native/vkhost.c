/* Vulkan rendering for engines that offer it (PCEE2's default renderer, LRPS2's Vulkan option, later Android). The
   engine draws on a device made through libretro's context negotiation (version 1), on our instance and shared queue;
   each finished frame arrives through set_image. With a game window it is copied on the GPU into an image of ours (kept
   for pauses, saves and screenshots) and blitted, scaled, into a swap chain: nothing comes back to the CPU. Without one
   it is read back one frame late, as the OpenGL and Direct3D paths do. The Vulkan loader is opened at run time; only the
   window surface is platform-specific (Win32 here; Android and desktop Linux add theirs). */
#include "vkhost.h"
#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#endif
#include <vulkan/vulkan.h>
#include "libretro.h"
#include "libretro_vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 2      /* frames in flight: sync indices offered to the engine */
#define MAX_SEMS 8

#define GLOBAL_FUNCS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define INSTANCE_FUNCS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define DEVICE_FUNCS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdCopyImageToBuffer) X(vkCmdClearColorImage) X(vkCmdCopyBufferToImage) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateBuffer) \
    X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)
#define DECLARE(name) static PFN_##name p_##name;
GLOBAL_FUNCS(DECLARE) INSTANCE_FUNCS(DECLARE) DEVICE_FUNCS(DECLARE)
#ifdef _WIN32
static PFN_vkCreateWin32SurfaceKHR p_vkCreateWin32SurfaceKHR;
static HMODULE library;
#else
static void *library;
#endif
static PFN_vkGetInstanceProcAddr get_instance_proc;

static const struct retro_hw_render_context_negotiation_interface_vulkan *negotiation;
static struct retro_hw_render_interface_vulkan iface;
static VkInstance instance;static VkPhysicalDevice gpu;static VkDevice device;static VkQueue queue;static uint32_t queue_family;
/* Set once the driver loses the device (a GPU fault or reset) or stops finishing work: nothing waits on it again, and
   the player ends the game instead of freezing. */
static int lost;
static int has_surface_extension,has_swapchain_extension,device_from_engine;
#ifdef _WIN32
static CRITICAL_SECTION queue_lock;
#else
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
#endif
static int lock_ready;
static VkCommandPool pool;static VkCommandBuffer cmd[FRAMES];static VkFence fence[FRAMES];static int submitted[FRAMES];
static VkSemaphore acquired[FRAMES],rendered[FRAMES];static uint32_t frame;
/* What the engine handed over for the frame being shown. */
static struct retro_vulkan_image image;static int have_image;
static VkSemaphore wait_sems[MAX_SEMS];static uint32_t wait_count;
static VkCommandBuffer engine_cmds[MAX_SEMS];static uint32_t engine_cmd_count;static VkSemaphore signal_sem;
/* Our copy of the last frame. */
static VkImage last;static VkDeviceMemory last_memory;static unsigned last_w,last_h;static VkFormat last_format;static int last_valid;
/* Readback ring (no window). */
static VkBuffer readback[FRAMES];static VkDeviceMemory readback_memory[FRAMES];static void *readback_map[FRAMES];
static VkDeviceSize readback_size[FRAMES];static unsigned readback_w[FRAMES],readback_h[FRAMES];static VkFormat readback_format[FRAMES];
static int readback_pending[FRAMES];static int warned_format;
/* Swap chain on the game window. */
static VkSurfaceKHR surface;static VkSwapchainKHR chain;static VkImage chain_images[8];static uint32_t chain_count;
static VkFormat chain_format;static VkExtent2D chain_extent;static VkPresentModeKHR chain_mode;
#ifdef _WIN32
static HWND window;
#endif
/* The player's overlay (achievement pop-ups, challenge and progress indicators, notices): a bitmap the size of the game
   picture whose pixels are either opaque or fully transparent. Each frame its opaque runs, one per row segment, are
   copied into the window image after the game's frame. Each frame in flight has its own staging buffer, rewritten only
   after that frame's fence has passed, so the GPU never reads a buffer while it changes. */
#define MAX_OVERLAY_RUNS 8192
static uint32_t *overlay_copy;static unsigned overlay_w,overlay_h,overlay_generation;
static VkBufferImageCopy overlay_runs[MAX_OVERLAY_RUNS],overlay_regions[MAX_OVERLAY_RUNS];static uint32_t overlay_run_count;
static VkBuffer overlay_buffer[FRAMES];static VkDeviceMemory overlay_memory[FRAMES];static void *overlay_map[FRAMES];
static VkDeviceSize overlay_size[FRAMES];static unsigned overlay_slot_generation[FRAMES];

static void lock_queue(void *handle){(void)handle;if(!lock_ready)return;
#ifdef _WIN32
    EnterCriticalSection(&queue_lock);
#else
    pthread_mutex_lock(&queue_lock);
#endif
}
static void unlock_queue(void *handle){(void)handle;if(!lock_ready)return;
#ifdef _WIN32
    LeaveCriticalSection(&queue_lock);
#else
    pthread_mutex_unlock(&queue_lock);
#endif
}
static void set_image(void *handle,const struct retro_vulkan_image *img,uint32_t num_semaphores,const VkSemaphore *semaphores,uint32_t src_queue_family){
    (void)handle;(void)src_queue_family;
    if(!img){have_image=0;return;}
    image=*img;have_image=1;wait_count=0;
    for(uint32_t i=0;i<num_semaphores&&i<MAX_SEMS;i++)wait_sems[wait_count++]=semaphores[i];
}
static uint32_t get_sync_index(void *handle){(void)handle;return frame;}
static uint32_t get_sync_index_mask(void *handle){(void)handle;return (1u<<FRAMES)-1;}
static void set_command_buffers(void *handle,uint32_t num,const VkCommandBuffer *cmds){
    (void)handle;engine_cmd_count=0;for(uint32_t i=0;i<num&&i<MAX_SEMS;i++)engine_cmds[engine_cmd_count++]=cmds[i];
}
#define FENCE_WAIT_NS 3000000000ull /* far beyond any frame; a longer wait means the GPU is stuck */
static void device_failed(VkResult r,const char *where){
    if(lost)return;
    lost=1;
    if(r==VK_ERROR_DEVICE_LOST)fprintf(stderr,"Vulkan: the graphics driver lost the device (%s); the game was stopped\n",where);
    else fprintf(stderr,"Vulkan: the graphics card stopped responding (%s, %d); the game was stopped\n",where,(int)r);
}
/* Waits for a sync index's work, never forever. */
static int wait_fence(int index,const char *where){
    if(lost)return 0;
    if(!submitted[index])return 1;
    VkResult r=p_vkWaitForFences(device,1,&fence[index],VK_TRUE,FENCE_WAIT_NS);
    if(r!=VK_SUCCESS){device_failed(r,where);return 0;}
    submitted[index]=0;
    return 1;
}
static VkResult queue_idle(void){
    if(lost)return VK_ERROR_DEVICE_LOST;
    /* vkQueueWaitIdle has no timeout; waiting on the in-flight fences does, and covers everything we submitted. */
    for(int i=0;i<FRAMES;i++)if(!wait_fence(i,"waiting for the graphics card"))return VK_ERROR_DEVICE_LOST;
    lock_queue(NULL);VkResult r=p_vkQueueWaitIdle(queue);unlock_queue(NULL);
    if(r!=VK_SUCCESS)device_failed(r,"waiting for the graphics card");
    return r;
}
static void wait_sync_index(void *handle){(void)handle;wait_fence((int)frame,"engine frame");}
static void set_signal_semaphore(void *handle,VkSemaphore semaphore){(void)handle;signal_sem=semaphore;}

void eh_vk_set_negotiation(const void *data){negotiation=data;}
const void *eh_vk_interface(void){return device?&iface:NULL;}

static int memory_type(uint32_t bits,VkMemoryPropertyFlags want){
    VkPhysicalDeviceMemoryProperties props;p_vkGetPhysicalDeviceMemoryProperties(gpu,&props);
    for(uint32_t i=0;i<props.memoryTypeCount;i++)if((bits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&want)==want)return (int)i;
    return -1;
}
static void barrier(VkCommandBuffer c,VkImage img,VkImageLayout from,VkImageLayout to,VkAccessFlags src_access,VkAccessFlags dst_access,VkPipelineStageFlags src_stage,VkPipelineStageFlags dst_stage){
    VkImageMemoryBarrier b;memset(&b,0,sizeof(b));b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask=src_access;b.dstAccessMask=dst_access;b.oldLayout=from;b.newLayout=to;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=img;
    b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;b.subresourceRange.levelCount=1;b.subresourceRange.layerCount=1;
    p_vkCmdPipelineBarrier(c,src_stage,dst_stage,0,0,NULL,0,NULL,1,&b);
}
static int convert(VkFormat format,const uint8_t *src,uint32_t *dst,unsigned w){
    switch(format){
    case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_R8G8B8A8_SRGB:case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        for(unsigned x=0;x<w;x++,src+=4){dst[x]=((uint32_t)src[0]<<16)|((uint32_t)src[1]<<8)|src[2];}return 1;
    case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_B8G8R8A8_SRGB:
        for(unsigned x=0;x<w;x++,src+=4){dst[x]=((uint32_t)src[2]<<16)|((uint32_t)src[1]<<8)|src[0];}return 1;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        for(unsigned x=0;x<w;x++,src+=4){uint32_t v;memcpy(&v,src,4);dst[x]=(((v>>2)&255)<<16)|(((v>>12)&255)<<8)|((v>>22)&255);}return 1;
    default:
        if(!warned_format){warned_format=1;fprintf(stderr,"Vulkan: frame format %d is not supported\n",(int)format);}
        return 0;
    }
}

int eh_vk_create(void){
    if(device)return 1;
#ifdef _WIN32
    library=LoadLibraryExW(L"vulkan-1.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(library){FARPROC f=GetProcAddress(library,"vkGetInstanceProcAddr");memcpy(&get_instance_proc,&f,sizeof(f));}
#else
    library=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);if(!library)library=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
    if(library){void *f=dlsym(library,"vkGetInstanceProcAddr");memcpy(&get_instance_proc,&f,sizeof(f));}
#endif
    if(!get_instance_proc){fprintf(stderr,"Vulkan is not installed on this computer\n");return 0;}
#define LOAD_GLOBAL(name) {PFN_vkVoidFunction f=get_instance_proc(VK_NULL_HANDLE,#name);memcpy(&p_##name,&f,sizeof(f));}
    GLOBAL_FUNCS(LOAD_GLOBAL)
    if(!p_vkCreateInstance)return 0;
    PFN_vkEnumerateInstanceVersion enumerate_version;{PFN_vkVoidFunction f=get_instance_proc(VK_NULL_HANDLE,"vkEnumerateInstanceVersion");memcpy(&enumerate_version,&f,sizeof(f));}
    uint32_t loader_version=VK_API_VERSION_1_0;if(enumerate_version)enumerate_version(&loader_version);
    if(loader_version<VK_API_VERSION_1_1){fprintf(stderr,"Vulkan 1.1 is required; this computer has %u.%u\n",VK_VERSION_MAJOR(loader_version),VK_VERSION_MINOR(loader_version));return 0;}
    /* Surface extensions, when the loader has them, so the game window can show frames. */
    const char *extensions[2];uint32_t extension_count=0,available=0;
    p_vkEnumerateInstanceExtensionProperties(NULL,&available,NULL);
    VkExtensionProperties *props=calloc(available?available:1,sizeof(*props));
    if(props){p_vkEnumerateInstanceExtensionProperties(NULL,&available,props);}
    int surface_ext=0,platform_ext=0;
    for(uint32_t i=0;props&&i<available;i++){
        if(!strcmp(props[i].extensionName,VK_KHR_SURFACE_EXTENSION_NAME))surface_ext=1;
#ifdef _WIN32
        if(!strcmp(props[i].extensionName,VK_KHR_WIN32_SURFACE_EXTENSION_NAME))platform_ext=1;
#endif
    }
    free(props);
    if(surface_ext&&platform_ext){
        extensions[extension_count++]=VK_KHR_SURFACE_EXTENSION_NAME;
#ifdef _WIN32
        extensions[extension_count++]=VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#endif
        has_surface_extension=1;
    }
    VkApplicationInfo app;memset(&app,0,sizeof(app));app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName="EmuHall";app.pEngineName="EmuHall";app.apiVersion=VK_API_VERSION_1_1;
    if(negotiation&&negotiation->get_application_info){const VkApplicationInfo *engine_app=negotiation->get_application_info();if(engine_app)app=*engine_app;}
    if(app.apiVersion<VK_API_VERSION_1_1)app.apiVersion=VK_API_VERSION_1_1;
    if(app.apiVersion>loader_version)app.apiVersion=loader_version;
    VkInstanceCreateInfo ici;memset(&ici,0,sizeof(ici));ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo=&app;ici.enabledExtensionCount=extension_count;ici.ppEnabledExtensionNames=extensions;
    VkResult r=p_vkCreateInstance(&ici,NULL,&instance);
    if(r!=VK_SUCCESS){fprintf(stderr,"Vulkan: no instance (%d)\n",(int)r);return 0;}
#define LOAD_INSTANCE(name) {PFN_vkVoidFunction f=get_instance_proc(instance,#name);memcpy(&p_##name,&f,sizeof(f));}
    INSTANCE_FUNCS(LOAD_INSTANCE)
#ifdef _WIN32
    {PFN_vkVoidFunction f=get_instance_proc(instance,"vkCreateWin32SurfaceKHR");memcpy(&p_vkCreateWin32SurfaceKHR,&f,sizeof(f));}
#endif
    /* A dedicated GPU when there is one, else the integrated one. */
    uint32_t gpu_count=0;p_vkEnumeratePhysicalDevices(instance,&gpu_count,NULL);
    VkPhysicalDevice gpus[16];if(gpu_count>16)gpu_count=16;
    if(!gpu_count||p_vkEnumeratePhysicalDevices(instance,&gpu_count,gpus)<0){fprintf(stderr,"Vulkan: no graphics card\n");return 0;}
    int best=-1,best_score=-1;
    for(uint32_t i=0;i<gpu_count;i++){
        VkPhysicalDeviceProperties p;p_vkGetPhysicalDeviceProperties(gpus[i],&p);
        int score=p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?3:p.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU?2:p.deviceType==VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU?1:0;
        if(p.apiVersion>=VK_API_VERSION_1_1&&score>best_score){best=(int)i;best_score=score;}
    }
    if(best<0){fprintf(stderr,"Vulkan: no graphics card supports Vulkan 1.1\n");return 0;}
    gpu=gpus[best];
    {uint32_t n=0;p_vkEnumerateDeviceExtensionProperties(gpu,NULL,&n,NULL);VkExtensionProperties *d=calloc(n?n:1,sizeof(*d));
     if(d){p_vkEnumerateDeviceExtensionProperties(gpu,NULL,&n,d);for(uint32_t i=0;i<n;i++)if(!strcmp(d[i].extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME))has_swapchain_extension=1;free(d);}}
    /* Static: engines keep this list and read it again later (PCEE2 checks it whenever PCSX2 reopens its graphics). */
    static const char *device_extensions[1];unsigned device_extension_count=0;
    if(has_surface_extension&&has_swapchain_extension)device_extensions[device_extension_count++]=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if(negotiation&&negotiation->create_device){
        /* The engine makes the device with what it needs plus what we ask for. */
        struct retro_vulkan_context context;memset(&context,0,sizeof(context));
        if(!negotiation->create_device(&context,instance,gpu,VK_NULL_HANDLE,get_instance_proc,device_extensions,device_extension_count,NULL,0,NULL)||!context.device){
            fprintf(stderr,"Vulkan: the engine could not create its device\n");return 0;
        }
        gpu=context.gpu?context.gpu:gpu;device=context.device;queue=context.queue;queue_family=context.queue_family_index;device_from_engine=1;
    }else{
        uint32_t families=0;p_vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,NULL);
        VkQueueFamilyProperties fam[16];if(families>16)families=16;p_vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,fam);
        int found=-1;for(uint32_t i=0;i<families;i++)if(fam[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){found=(int)i;break;}
        if(found<0){fprintf(stderr,"Vulkan: no graphics queue\n");return 0;}
        float priority=1.0f;VkDeviceQueueCreateInfo q;memset(&q,0,sizeof(q));q.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q.queueFamilyIndex=(uint32_t)found;q.queueCount=1;q.pQueuePriorities=&priority;
        VkDeviceCreateInfo dci;memset(&dci,0,sizeof(dci));dci.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&q;dci.enabledExtensionCount=device_extension_count;dci.ppEnabledExtensionNames=device_extensions;
        if(p_vkCreateDevice(gpu,&dci,NULL,&device)!=VK_SUCCESS){fprintf(stderr,"Vulkan: no device\n");device=VK_NULL_HANDLE;return 0;}
        queue_family=(uint32_t)found;
    }
#define LOAD_DEVICE(name) {PFN_vkVoidFunction f=p_vkGetDeviceProcAddr(device,#name);memcpy(&p_##name,&f,sizeof(f));}
    DEVICE_FUNCS(LOAD_DEVICE)
    if(!queue)p_vkGetDeviceQueue(device,queue_family,0,&queue);
#ifdef _WIN32
    InitializeCriticalSection(&queue_lock);
#endif
    lock_ready=1;
    VkCommandPoolCreateInfo pci;memset(&pci,0,sizeof(pci));pci.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pci.queueFamilyIndex=queue_family;
    if(p_vkCreateCommandPool(device,&pci,NULL,&pool)!=VK_SUCCESS)return 0;
    VkCommandBufferAllocateInfo cai;memset(&cai,0,sizeof(cai));cai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=FRAMES;
    if(p_vkAllocateCommandBuffers(device,&cai,cmd)!=VK_SUCCESS)return 0;
    for(int i=0;i<FRAMES;i++){
        VkFenceCreateInfo fci;memset(&fci,0,sizeof(fci));fci.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkSemaphoreCreateInfo sci;memset(&sci,0,sizeof(sci));sci.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if(p_vkCreateFence(device,&fci,NULL,&fence[i])!=VK_SUCCESS||p_vkCreateSemaphore(device,&sci,NULL,&acquired[i])!=VK_SUCCESS||
           p_vkCreateSemaphore(device,&sci,NULL,&rendered[i])!=VK_SUCCESS)return 0;
    }
    memset(&iface,0,sizeof(iface));
    iface.interface_type=RETRO_HW_RENDER_INTERFACE_VULKAN;iface.interface_version=RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    iface.handle=&iface;iface.instance=instance;iface.gpu=gpu;iface.device=device;iface.get_device_proc_addr=p_vkGetDeviceProcAddr;
    iface.get_instance_proc_addr=get_instance_proc;iface.queue=queue;iface.queue_index=queue_family;
    iface.set_image=set_image;iface.get_sync_index=get_sync_index;iface.get_sync_index_mask=get_sync_index_mask;
    iface.set_command_buffers=set_command_buffers;iface.wait_sync_index=wait_sync_index;iface.lock_queue=lock_queue;
    iface.unlock_queue=unlock_queue;iface.set_signal_semaphore=set_signal_semaphore;
    VkPhysicalDeviceProperties p;p_vkGetPhysicalDeviceProperties(gpu,&p);
    fprintf(stderr,"Vulkan %u.%u: %s%s\n",VK_VERSION_MAJOR(app.apiVersion),VK_VERSION_MINOR(app.apiVersion),p.deviceName,device_from_engine?" (device made by the engine)":"");
    return 1;
}

/* Waits for the work this sync index carried last time and starts recording into its command buffer. */
static int begin_frame(void){
    if(!wait_fence((int)frame,"frame"))return 0;
    p_vkResetFences(device,1,&fence[frame]);
    p_vkResetCommandBuffer(cmd[frame],0);
    VkCommandBufferBeginInfo bi;memset(&bi,0,sizeof(bi));bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return p_vkBeginCommandBuffer(cmd[frame],&bi)==VK_SUCCESS;
}
/* Submits after the engine's own command buffers, waiting on its semaphores (and the swap chain image when given). */
static int end_frame(VkSemaphore wait_acquired,VkSemaphore signal_rendered){
    if(p_vkEndCommandBuffer(cmd[frame])!=VK_SUCCESS)return 0;
    VkSemaphore waits[MAX_SEMS+1];VkPipelineStageFlags stages[MAX_SEMS+1];uint32_t nw=0;
    for(uint32_t i=0;i<wait_count;i++){waits[nw]=wait_sems[i];stages[nw++]=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;}
    if(wait_acquired){waits[nw]=wait_acquired;stages[nw++]=VK_PIPELINE_STAGE_TRANSFER_BIT;}
    VkCommandBuffer buffers[MAX_SEMS+1];uint32_t nb=0;
    for(uint32_t i=0;i<engine_cmd_count;i++)buffers[nb++]=engine_cmds[i];
    buffers[nb++]=cmd[frame];
    VkSemaphore signals[2];uint32_t ns=0;if(signal_rendered)signals[ns++]=signal_rendered;if(signal_sem)signals[ns++]=signal_sem;
    VkSubmitInfo si;memset(&si,0,sizeof(si));si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount=nw;si.pWaitSemaphores=waits;si.pWaitDstStageMask=stages;si.commandBufferCount=nb;si.pCommandBuffers=buffers;
    si.signalSemaphoreCount=ns;si.pSignalSemaphores=signals;
    lock_queue(NULL);VkResult r=p_vkQueueSubmit(queue,1,&si,fence[frame]);unlock_queue(NULL);
    wait_count=0;engine_cmd_count=0;signal_sem=VK_NULL_HANDLE;
    if(r!=VK_SUCCESS){device_failed(r,"submit");return 0;}
    submitted[frame]=1;
    return 1;
}
static void overlay_take(const uint32_t *pixels,unsigned w,unsigned h){
    size_t n=(size_t)w*h;
    if(!overlay_copy||overlay_w!=w||overlay_h!=h){free(overlay_copy);overlay_copy=malloc(n*4);overlay_w=overlay_copy?w:0;overlay_h=overlay_copy?h:0;}
    overlay_run_count=0;overlay_generation++;
    if(!overlay_copy)return;
    memcpy(overlay_copy,pixels,n*4);
    for(unsigned y=0;y<h;y++){
        const uint32_t *row=pixels+(size_t)y*w;unsigned x=0;
        while(x<w){
            while(x<w&&!(row[x]>>24))x++;
            unsigned start=x;while(x<w&&(row[x]>>24))x++;
            if(x>start){
                if(overlay_run_count==MAX_OVERLAY_RUNS)return;
                VkBufferImageCopy *r=&overlay_runs[overlay_run_count++];memset(r,0,sizeof(*r));
                r->bufferOffset=((VkDeviceSize)y*w+start)*4;r->imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;r->imageSubresource.layerCount=1;
                r->imageOffset.x=(int32_t)start;r->imageOffset.y=(int32_t)y;r->imageExtent.width=x-start;r->imageExtent.height=1;r->imageExtent.depth=1;
            }
        }
    }
}
/* Records the overlay copy into the window image (in TRANSFER_DST) at the picture's position. A failure to set up the
   staging buffer only leaves the overlay out. */
static void overlay_record(VkCommandBuffer c,VkImage target,int x,int y){
    if(!overlay_run_count||!overlay_copy)return;
    int swap=chain_format==VK_FORMAT_R8G8B8A8_UNORM||chain_format==VK_FORMAT_R8G8B8A8_SRGB;
    if(!swap&&chain_format!=VK_FORMAT_B8G8R8A8_UNORM&&chain_format!=VK_FORMAT_B8G8R8A8_SRGB)return;
    uint32_t slot=frame;VkDeviceSize size=(VkDeviceSize)overlay_w*overlay_h*4;
    if(!overlay_buffer[slot]||overlay_size[slot]<size){
        if(overlay_buffer[slot]){p_vkUnmapMemory(device,overlay_memory[slot]);p_vkDestroyBuffer(device,overlay_buffer[slot],NULL);p_vkFreeMemory(device,overlay_memory[slot],NULL);overlay_buffer[slot]=VK_NULL_HANDLE;overlay_size[slot]=0;}
        VkBufferCreateInfo bci;memset(&bci,0,sizeof(bci));bci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bci.size=size;bci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if(p_vkCreateBuffer(device,&bci,NULL,&overlay_buffer[slot])!=VK_SUCCESS){overlay_buffer[slot]=VK_NULL_HANDLE;return;}
        VkMemoryRequirements req;p_vkGetBufferMemoryRequirements(device,overlay_buffer[slot],&req);
        int type=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo ai;memset(&ai,0,sizeof(ai));ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=req.size;ai.memoryTypeIndex=(uint32_t)type;
        if(type<0||p_vkAllocateMemory(device,&ai,NULL,&overlay_memory[slot])!=VK_SUCCESS){p_vkDestroyBuffer(device,overlay_buffer[slot],NULL);overlay_buffer[slot]=VK_NULL_HANDLE;return;}
        if(p_vkBindBufferMemory(device,overlay_buffer[slot],overlay_memory[slot],0)!=VK_SUCCESS||p_vkMapMemory(device,overlay_memory[slot],0,VK_WHOLE_SIZE,0,&overlay_map[slot])!=VK_SUCCESS){
            p_vkDestroyBuffer(device,overlay_buffer[slot],NULL);p_vkFreeMemory(device,overlay_memory[slot],NULL);overlay_buffer[slot]=VK_NULL_HANDLE;return;
        }
        overlay_size[slot]=size;overlay_slot_generation[slot]=overlay_generation-1;
    }
    if(overlay_slot_generation[slot]!=overlay_generation){
        uint32_t *dst=overlay_map[slot];size_t n=(size_t)overlay_w*overlay_h;
        if(swap){for(size_t i=0;i<n;i++){uint32_t p=overlay_copy[i];dst[i]=(p&0xff00ff00u)|((p>>16)&0xffu)|((p&0xffu)<<16);}}
        else memcpy(dst,overlay_copy,n*4);
        overlay_slot_generation[slot]=overlay_generation;
    }
    for(uint32_t i=0;i<overlay_run_count;i++){overlay_regions[i]=overlay_runs[i];overlay_regions[i].imageOffset.x+=x;overlay_regions[i].imageOffset.y+=y;}
    barrier(c,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    p_vkCmdCopyBufferToImage(c,overlay_buffer[slot],target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,overlay_run_count,overlay_regions);
}
static int ensure_last(unsigned w,unsigned h,VkFormat format){
    if(last&&last_w==w&&last_h==h&&last_format==format)return 1;
    if(last){if(queue_idle()!=VK_SUCCESS)return 0;p_vkDestroyImage(device,last,NULL);p_vkFreeMemory(device,last_memory,NULL);last=VK_NULL_HANDLE;}
    VkImageCreateInfo ci;memset(&ci,0,sizeof(ci));ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ci.imageType=VK_IMAGE_TYPE_2D;
    ci.format=format;ci.extent.width=w;ci.extent.height=h;ci.extent.depth=1;ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;
    ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(p_vkCreateImage(device,&ci,NULL,&last)!=VK_SUCCESS){last=VK_NULL_HANDLE;return 0;}
    VkMemoryRequirements req;p_vkGetImageMemoryRequirements(device,last,&req);
    int type=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);if(type<0)type=memory_type(req.memoryTypeBits,0);
    VkMemoryAllocateInfo ai;memset(&ai,0,sizeof(ai));ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=req.size;ai.memoryTypeIndex=(uint32_t)type;
    if(type<0||p_vkAllocateMemory(device,&ai,NULL,&last_memory)!=VK_SUCCESS||p_vkBindImageMemory(device,last,last_memory,0)!=VK_SUCCESS){
        p_vkDestroyImage(device,last,NULL);last=VK_NULL_HANDLE;return 0;
    }
    last_w=w;last_h=h;last_format=format;last_valid=0;
    return 1;
}
/* Records the copy of the engine's frame into our image; leaves ours in TRANSFER_SRC. */
static void record_copy_to_last(VkCommandBuffer c,unsigned w,unsigned h){
    VkImage src=image.create_info.image;
    barrier(c,src,image.image_layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(c,last,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy region;memset(&region,0,sizeof(region));
    region.srcSubresource.aspectMask=region.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;region.srcSubresource.layerCount=region.dstSubresource.layerCount=1;
    region.extent.width=w;region.extent.height=h;region.extent.depth=1;
    p_vkCmdCopyImage(c,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,last,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    barrier(c,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,image.image_layout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier(c,last,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    last_valid=1;
}

int eh_vk_read(unsigned width,unsigned height,uint32_t *out,unsigned *out_w,unsigned *out_h){
    if(lost)return -1;
    if(!device||!have_image||!width||!height)return 0;
    have_image=0;
    if(!begin_frame())return -1;
    int got=0,slot=(int)frame;
    /* The copy this slot made FRAMES frames ago is finished: begin_frame waited for its fence. */
    if(readback_pending[slot]){
        readback_pending[slot]=0;int ok=1;
        for(unsigned y=0;y<readback_h[slot]&&ok;y++){ok=convert(readback_format[slot],(const uint8_t*)readback_map[slot]+(size_t)y*readback_w[slot]*4,out+(size_t)y*readback_w[slot],readback_w[slot]);}
        if(!ok){p_vkEndCommandBuffer(cmd[frame]);return -1;}
        *out_w=readback_w[slot];*out_h=readback_h[slot];got=1;
    }
    VkDeviceSize size=(VkDeviceSize)width*height*4;
    if(!readback[slot]||readback_size[slot]<size){
        if(readback[slot]){p_vkUnmapMemory(device,readback_memory[slot]);p_vkDestroyBuffer(device,readback[slot],NULL);p_vkFreeMemory(device,readback_memory[slot],NULL);readback[slot]=VK_NULL_HANDLE;}
        VkBufferCreateInfo bci;memset(&bci,0,sizeof(bci));bci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bci.size=size;bci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(p_vkCreateBuffer(device,&bci,NULL,&readback[slot])!=VK_SUCCESS)return -1;
        VkMemoryRequirements req;p_vkGetBufferMemoryRequirements(device,readback[slot],&req);
        int type=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if(type<0)type=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo ai;memset(&ai,0,sizeof(ai));ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=req.size;ai.memoryTypeIndex=(uint32_t)type;
        if(type<0||p_vkAllocateMemory(device,&ai,NULL,&readback_memory[slot])!=VK_SUCCESS||p_vkBindBufferMemory(device,readback[slot],readback_memory[slot],0)!=VK_SUCCESS||
           p_vkMapMemory(device,readback_memory[slot],0,VK_WHOLE_SIZE,0,&readback_map[slot])!=VK_SUCCESS)return -1;
        readback_size[slot]=size;
    }
    VkImage src=image.create_info.image;VkCommandBuffer c=cmd[frame];
    barrier(c,src,image.image_layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region;memset(&region,0,sizeof(region));region.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;region.imageSubresource.layerCount=1;
    region.imageExtent.width=width;region.imageExtent.height=height;region.imageExtent.depth=1;
    p_vkCmdCopyImageToBuffer(c,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback[slot],1,&region);
    barrier(c,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,image.image_layout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    if(!end_frame(VK_NULL_HANDLE,VK_NULL_HANDLE))return -1;
    readback_w[slot]=width;readback_h[slot]=height;readback_format[slot]=image.create_info.format;readback_pending[slot]=1;
    frame=(frame+1)%FRAMES;
    return got;
}

static void destroy_chain(void){
    if(chain){p_vkDestroySwapchainKHR(device,chain,NULL);chain=VK_NULL_HANDLE;}
    chain_count=0;
}
static int create_chain(unsigned width,unsigned height){
    VkSurfaceCapabilitiesKHR caps;if(p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu,surface,&caps)!=VK_SUCCESS)return 0;
    if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT)){fprintf(stderr,"Vulkan: the window cannot be drawn into by copies\n");return 0;}
    VkExtent2D extent=caps.currentExtent;if(extent.width==0xFFFFFFFFu){extent.width=width;extent.height=height;}
    if(!extent.width||!extent.height)return 1; /* minimised: try again later */
    uint32_t count=caps.minImageCount<3?3:caps.minImageCount;if(caps.maxImageCount&&count>caps.maxImageCount)count=caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci;memset(&ci,0,sizeof(ci));ci.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;ci.surface=surface;
    ci.minImageCount=count;ci.imageFormat=chain_format;ci.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;ci.imageExtent=extent;
    ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform=caps.currentTransform;ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;ci.presentMode=chain_mode;ci.clipped=VK_TRUE;
    ci.oldSwapchain=chain;
    VkSwapchainKHR fresh;if(p_vkCreateSwapchainKHR(device,&ci,NULL,&fresh)!=VK_SUCCESS)return 0;
    destroy_chain();chain=fresh;
    chain_count=8;if(p_vkGetSwapchainImagesKHR(device,chain,&chain_count,chain_images)<0){destroy_chain();return 0;}
    chain_extent=extent;
    return 1;
}
int eh_vk_present_attach(void *win){
    if(!device||!win||!has_surface_extension||!has_swapchain_extension)return 0;
#ifdef _WIN32
    window=(HWND)win;
    VkWin32SurfaceCreateInfoKHR sci;memset(&sci,0,sizeof(sci));sci.sType=VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance=GetModuleHandleW(NULL);sci.hwnd=window;
    if(!p_vkCreateWin32SurfaceKHR||p_vkCreateWin32SurfaceKHR(instance,&sci,NULL,&surface)!=VK_SUCCESS){surface=VK_NULL_HANDLE;return 0;}
#else
    return 0; /* Desktop Linux and Android add their surfaces here. */
#endif
    VkBool32 supported=VK_FALSE;p_vkGetPhysicalDeviceSurfaceSupportKHR(gpu,queue_family,surface,&supported);
    if(!supported){fprintf(stderr,"Vulkan: the engine's queue cannot show frames; they are copied instead\n");p_vkDestroySurfaceKHR(instance,surface,NULL);surface=VK_NULL_HANDLE;return 0;}
    uint32_t n=0;p_vkGetPhysicalDeviceSurfaceFormatsKHR(gpu,surface,&n,NULL);VkSurfaceFormatKHR formats[64];if(n>64)n=64;
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(gpu,surface,&n,formats);chain_format=n?formats[0].format:VK_FORMAT_B8G8R8A8_UNORM;
    for(uint32_t i=0;i<n;i++)if(formats[i].format==VK_FORMAT_B8G8R8A8_UNORM||formats[i].format==VK_FORMAT_R8G8B8A8_UNORM){chain_format=formats[i].format;break;}
    /* Never wait inside the engine's frame: mailbox, else immediate, else FIFO. */
    uint32_t m=0;p_vkGetPhysicalDeviceSurfacePresentModesKHR(gpu,surface,&m,NULL);VkPresentModeKHR modes[16];if(m>16)m=16;
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(gpu,surface,&m,modes);chain_mode=VK_PRESENT_MODE_FIFO_KHR;
    for(uint32_t i=0;i<m;i++)if(modes[i]==VK_PRESENT_MODE_MAILBOX_KHR)chain_mode=VK_PRESENT_MODE_MAILBOX_KHR;
    if(chain_mode!=VK_PRESENT_MODE_MAILBOX_KHR)for(uint32_t i=0;i<m;i++)if(modes[i]==VK_PRESENT_MODE_IMMEDIATE_KHR)chain_mode=VK_PRESENT_MODE_IMMEDIATE_KHR;
#ifdef _WIN32
    RECT rc;GetClientRect(window,&rc);
    if(!create_chain((unsigned)rc.right,(unsigned)rc.bottom)){p_vkDestroySurfaceKHR(instance,surface,NULL);surface=VK_NULL_HANDLE;return 0;}
#endif
    fprintf(stderr,"Vulkan: frames are shown on the graphics card (%s)\n",chain_mode==VK_PRESENT_MODE_MAILBOX_KHR?"mailbox":chain_mode==VK_PRESENT_MODE_IMMEDIATE_KHR?"immediate":"fifo");
    return 1;
}
int eh_vk_present(unsigned frame_w,unsigned frame_h,int smooth,int integer_scale,const uint32_t *overlay,unsigned overlay_width,unsigned overlay_height,int overlay_changed,int *view_x,int *view_y,int *view_w,int *view_h){
    if(lost)return -1;
    if(!overlay){if(overlay_run_count){overlay_run_count=0;overlay_generation++;}}
    else if(overlay_changed||overlay_width!=overlay_w||overlay_height!=overlay_h)overlay_take(overlay,overlay_width,overlay_height);
    if(!surface)return 0;
    if(!have_image)return 1;
    have_image=0;
    VkImageViewCreateInfo *info=&image.create_info;
    if(!frame_w||!frame_h||!ensure_last(frame_w,frame_h,info->format))return -1;
#ifdef _WIN32
    RECT rc;GetClientRect(window,&rc);unsigned cw=(unsigned)rc.right,ch=(unsigned)rc.bottom;
#else
    unsigned cw=chain_extent.width,ch=chain_extent.height;
#endif
    if(!cw||!ch)return 1;
    if(!chain||cw!=chain_extent.width||ch!=chain_extent.height){
        if(queue_idle()!=VK_SUCCESS||!create_chain(cw,ch))return -1;
        if(!chain)return 1;
    }
    if(!begin_frame())return -1;
    uint32_t index=0;VkResult r=p_vkAcquireNextImageKHR(device,chain,100000000ull,acquired[frame],VK_NULL_HANDLE,&index);
    if(r==VK_ERROR_OUT_OF_DATE_KHR||r==VK_TIMEOUT||r==VK_NOT_READY){
        /* Keep the engine's frame (for captures) and try the window again next frame. */
        record_copy_to_last(cmd[frame],frame_w,frame_h);
        if(!end_frame(VK_NULL_HANDLE,VK_NULL_HANDLE))return -1;
        frame=(frame+1)%FRAMES;
        if(r==VK_ERROR_OUT_OF_DATE_KHR){if(queue_idle()!=VK_SUCCESS)return -1;create_chain(cw,ch);}
        return 1;
    }
    if(r!=VK_SUCCESS&&r!=VK_SUBOPTIMAL_KHR){p_vkEndCommandBuffer(cmd[frame]);device_failed(r,"window image");return -1;}
    VkCommandBuffer c=cmd[frame];
    record_copy_to_last(c,frame_w,frame_h);
    /* Letterboxed exactly like the CPU path (present_frame in host.c). */
    int dw=(int)chain_extent.width,dh=(int)((long long)dw*frame_h/frame_w);
    if(dh>(int)chain_extent.height){dh=(int)chain_extent.height;dw=(int)((long long)dh*frame_w/frame_h);}
    if(integer_scale){int sx=(int)(chain_extent.width/frame_w),sy=(int)(chain_extent.height/frame_h),s=sx<sy?sx:sy;if(s>=1){dw=(int)frame_w*s;dh=(int)frame_h*s;}}
    int x=((int)chain_extent.width-dw)/2,y=((int)chain_extent.height-dh)/2;*view_x=x;*view_y=y;*view_w=dw;*view_h=dh;
    VkImage target=chain_images[index];
    barrier(c,target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black;memset(&black,0,sizeof(black));black.float32[3]=1.0f;
    VkImageSubresourceRange range;memset(&range,0,sizeof(range));range.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;range.levelCount=1;range.layerCount=1;
    p_vkCmdClearColorImage(c,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
    barrier(c,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit;memset(&blit,0,sizeof(blit));
    blit.srcSubresource.aspectMask=blit.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;blit.srcSubresource.layerCount=blit.dstSubresource.layerCount=1;
    blit.srcOffsets[1].x=(int32_t)frame_w;blit.srcOffsets[1].y=(int32_t)frame_h;blit.srcOffsets[1].z=1;
    blit.dstOffsets[0].x=x;blit.dstOffsets[0].y=y;blit.dstOffsets[1].x=x+dw;blit.dstOffsets[1].y=y+dh;blit.dstOffsets[1].z=1;
    p_vkCmdBlitImage(c,last,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,smooth?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
    /* The overlay is drawn for the picture size of the previous frames; it shows once it matches this one. */
    if(overlay&&overlay_w==(unsigned)dw&&overlay_h==(unsigned)dh)overlay_record(c,target,x,y);
    barrier(c,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if(!end_frame(acquired[frame],rendered[frame]))return -1;
    VkPresentInfoKHR pi;memset(&pi,0,sizeof(pi));pi.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&rendered[frame];pi.swapchainCount=1;pi.pSwapchains=&chain;pi.pImageIndices=&index;
    lock_queue(NULL);r=p_vkQueuePresentKHR(queue,&pi);unlock_queue(NULL);
    frame=(frame+1)%FRAMES;
    if(r==VK_ERROR_OUT_OF_DATE_KHR){if(queue_idle()!=VK_SUCCESS)return -1;create_chain(cw,ch);}
    else if(r!=VK_SUCCESS&&r!=VK_SUBOPTIMAL_KHR){device_failed(r,"present");return -1;}
    return 1;
}
int eh_vk_capture(uint32_t *out,unsigned *out_w,unsigned *out_h){
    if(lost||!device||!last||!last_valid)return 0;
    VkDeviceSize size=(VkDeviceSize)last_w*last_h*4;VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;void *map=NULL;int ok=0;
    VkBufferCreateInfo bci;memset(&bci,0,sizeof(bci));bci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bci.size=size;bci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if(p_vkCreateBuffer(device,&bci,NULL,&buffer)!=VK_SUCCESS)return 0;
    VkMemoryRequirements req;p_vkGetBufferMemoryRequirements(device,buffer,&req);
    int type=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai;memset(&ai,0,sizeof(ai));ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=req.size;ai.memoryTypeIndex=(uint32_t)type;
    VkCommandBuffer c=VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai;memset(&cai,0,sizeof(cai));cai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
    if(type>=0&&p_vkAllocateMemory(device,&ai,NULL,&memory)==VK_SUCCESS&&p_vkBindBufferMemory(device,buffer,memory,0)==VK_SUCCESS&&
       p_vkAllocateCommandBuffers(device,&cai,&c)==VK_SUCCESS){
        VkCommandBufferBeginInfo bi;memset(&bi,0,sizeof(bi));bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        p_vkBeginCommandBuffer(c,&bi);
        VkBufferImageCopy region;memset(&region,0,sizeof(region));region.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;region.imageSubresource.layerCount=1;
        region.imageExtent.width=last_w;region.imageExtent.height=last_h;region.imageExtent.depth=1;
        p_vkCmdCopyImageToBuffer(c,last,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&region);
        p_vkEndCommandBuffer(c);
        VkSubmitInfo si;memset(&si,0,sizeof(si));si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;si.commandBufferCount=1;si.pCommandBuffers=&c;
        lock_queue(NULL);VkResult r=p_vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE);unlock_queue(NULL);
        if(r==VK_SUCCESS)r=queue_idle();else device_failed(r,"screenshot");
        if(r==VK_SUCCESS&&p_vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&map)==VK_SUCCESS){
            ok=1;for(unsigned y=0;y<last_h&&ok;y++){ok=convert(last_format,(const uint8_t*)map+(size_t)y*last_w*4,out+(size_t)y*last_w,last_w);}
            p_vkUnmapMemory(device,memory);
        }
        p_vkFreeCommandBuffers(device,pool,1,&c);
    }
    if(memory){p_vkFreeMemory(device,memory,NULL);}
    p_vkDestroyBuffer(device,buffer,NULL);
    if(ok){*out_w=last_w;*out_h=last_h;}
    return ok;
}
int eh_vk_lost(void){return lost;}
void eh_vk_destroy(void){
    free(overlay_copy);overlay_copy=NULL;overlay_w=overlay_h=0;overlay_run_count=0;
    if(device&&lost){
        /* A lost device can block every call that waits on it, the engine's included: leave it to the process exit. */
        device=VK_NULL_HANDLE;instance=VK_NULL_HANDLE;
    }
    if(device){
        queue_idle();
        destroy_chain();
        if(surface){p_vkDestroySurfaceKHR(instance,surface,NULL);surface=VK_NULL_HANDLE;}
        if(last){p_vkDestroyImage(device,last,NULL);p_vkFreeMemory(device,last_memory,NULL);last=VK_NULL_HANDLE;}
        for(int i=0;i<FRAMES;i++){
            if(readback[i]){p_vkUnmapMemory(device,readback_memory[i]);p_vkDestroyBuffer(device,readback[i],NULL);p_vkFreeMemory(device,readback_memory[i],NULL);readback[i]=VK_NULL_HANDLE;}
            readback_size[i]=0;readback_pending[i]=0;submitted[i]=0;
            if(overlay_buffer[i]){p_vkUnmapMemory(device,overlay_memory[i]);p_vkDestroyBuffer(device,overlay_buffer[i],NULL);p_vkFreeMemory(device,overlay_memory[i],NULL);overlay_buffer[i]=VK_NULL_HANDLE;}
            overlay_size[i]=0;
            if(fence[i]){p_vkDestroyFence(device,fence[i],NULL);fence[i]=VK_NULL_HANDLE;}
            if(acquired[i]){p_vkDestroySemaphore(device,acquired[i],NULL);acquired[i]=VK_NULL_HANDLE;}
            if(rendered[i]){p_vkDestroySemaphore(device,rendered[i],NULL);rendered[i]=VK_NULL_HANDLE;}
        }
        if(pool){p_vkDestroyCommandPool(device,pool,NULL);pool=VK_NULL_HANDLE;}
        /* As RetroArch does: the engine releases what it made on the device, then the device goes. */
        if(negotiation&&negotiation->destroy_device)negotiation->destroy_device();
        p_vkDestroyDevice(device,NULL);device=VK_NULL_HANDLE;
    }
    if(instance){p_vkDestroyInstance(instance,NULL);instance=VK_NULL_HANDLE;}
    have_image=0;frame=0;negotiation=NULL;
    /* The loader stays loaded: the engine may keep function pointers until it is unloaded. */
}

/* For the launcher's Automatic renderer choice (`emuhall-core-host --check-vulkan`): a loader without any driver (Arch's
   mesa installs none) fails instance creation, and a driver may list no device for this computer. Run on its own. */
int eh_vk_count_devices(void){
    PFN_vkGetInstanceProcAddr gip=NULL;
#ifdef _WIN32
    HMODULE lib=LoadLibraryExW(L"vulkan-1.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(lib){FARPROC f=GetProcAddress(lib,"vkGetInstanceProcAddr");memcpy(&gip,&f,sizeof(f));}
#else
    void *lib=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);if(!lib)lib=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
    if(lib){void *f=dlsym(lib,"vkGetInstanceProcAddr");memcpy(&gip,&f,sizeof(f));}
#endif
    if(!gip){fprintf(stderr,"Vulkan: no loader\n");return -1;}
    PFN_vkCreateInstance create;{PFN_vkVoidFunction f=gip(VK_NULL_HANDLE,"vkCreateInstance");memcpy(&create,&f,sizeof(f));}
    if(!create)return -1;
    VkApplicationInfo app;memset(&app,0,sizeof(app));app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName="EmuHall";app.pEngineName="EmuHall";app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici;memset(&ici,0,sizeof(ici));ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ici.pApplicationInfo=&app;
    VkInstance probe;VkResult r=create(&ici,NULL,&probe);
    if(r!=VK_SUCCESS){fprintf(stderr,"Vulkan: no instance (%d)\n",(int)r);return 0;}
    PFN_vkEnumeratePhysicalDevices enumerate;PFN_vkGetPhysicalDeviceProperties properties;PFN_vkDestroyInstance destroy;
    {PFN_vkVoidFunction f=gip(probe,"vkEnumeratePhysicalDevices");memcpy(&enumerate,&f,sizeof(f));}
    {PFN_vkVoidFunction f=gip(probe,"vkGetPhysicalDeviceProperties");memcpy(&properties,&f,sizeof(f));}
    {PFN_vkVoidFunction f=gip(probe,"vkDestroyInstance");memcpy(&destroy,&f,sizeof(f));}
    int found=0;
    if(enumerate&&properties){
        VkPhysicalDevice gpus[16];uint32_t count=16;
        r=enumerate(probe,&count,gpus);
        for(uint32_t i=0;(r==VK_SUCCESS||r==VK_INCOMPLETE)&&i<count;i++){
            VkPhysicalDeviceProperties p;properties(gpus[i],&p);
            fprintf(stderr,"Vulkan device: %s (type %d, Vulkan %u.%u)\n",p.deviceName,(int)p.deviceType,VK_VERSION_MAJOR(p.apiVersion),VK_VERSION_MINOR(p.apiVersion));
            /* CPU drivers (lavapipe, which Ubuntu installs by default) do not count: OpenGL on the graphics card wins. */
            if(p.apiVersion>=VK_API_VERSION_1_1&&p.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU)found++;
        }
    }
    if(destroy)destroy(probe,NULL);
    return found;
}
