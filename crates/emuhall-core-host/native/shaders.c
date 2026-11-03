/* Original EmuHall presentation shaders, isolated from every emulator's GL context. */
#include "shaders.h"
#include "glhost.h"
#include "desktop.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
static HWND shader_window;static HDC shader_dc,previous_dc;static HGLRC shader_context,previous_context;
static int begin(void){
 previous_dc=wglGetCurrentDC();previous_context=wglGetCurrentContext();
 if(!shader_context){
  WNDCLASSW wc={0};wc.style=CS_OWNDC;wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(NULL);wc.lpszClassName=L"EmuHallShaders";RegisterClassW(&wc);
  shader_window=CreateWindowExW(0,wc.lpszClassName,L"EmuHall shaders",WS_POPUP,0,0,16,16,NULL,NULL,wc.hInstance,NULL);if(!shader_window)return 0;shader_dc=GetDC(shader_window);
  PIXELFORMATDESCRIPTOR pixel={0};pixel.nSize=sizeof(pixel);pixel.nVersion=1;pixel.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL;pixel.iPixelType=PFD_TYPE_RGBA;pixel.cColorBits=32;
  int format=ChoosePixelFormat(shader_dc,&pixel);if(!format||!SetPixelFormat(shader_dc,format,&pixel))return 0;
  HGLRC legacy=wglCreateContext(shader_dc);if(!legacy)return 0;if(!wglMakeCurrent(shader_dc,legacy)){wglDeleteContext(legacy);return 0;}
  PFNWGLCREATECONTEXTATTRIBSARBPROC create=(PFNWGLCREATECONTEXTATTRIBSARBPROC)(void*)wglGetProcAddress("wglCreateContextAttribsARB");
  if(create){int attributes[]={WGL_CONTEXT_MAJOR_VERSION_ARB,3,WGL_CONTEXT_MINOR_VERSION_ARB,3,WGL_CONTEXT_PROFILE_MASK_ARB,WGL_CONTEXT_CORE_PROFILE_BIT_ARB,0};shader_context=create(shader_dc,NULL,attributes);}
  wglMakeCurrent(previous_dc,previous_context);wglDeleteContext(legacy);if(!shader_context)return 0;
 }
 return wglMakeCurrent(shader_dc,shader_context);
}
static void end(void){wglMakeCurrent(previous_dc,previous_context);}
static void close_context(void){if(shader_context)wglDeleteContext(shader_context);shader_context=NULL;if(shader_window){ReleaseDC(shader_window,shader_dc);DestroyWindow(shader_window);}shader_window=NULL;shader_dc=NULL;}
#else
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>
static int begin(void){return eh_desktop_shader_begin();}
static void end(void){eh_desktop_shader_end();}
static void close_context(void){eh_desktop_shader_close();}
#endif
#define FUNCTIONS(X) \
 X(GLuint,glCreateShader,(GLenum)) X(void,glShaderSource,(GLuint,GLsizei,const GLchar*const*,const GLint*)) X(void,glCompileShader,(GLuint)) X(void,glGetShaderiv,(GLuint,GLenum,GLint*)) X(void,glDeleteShader,(GLuint)) \
 X(GLuint,glCreateProgram,(void)) X(void,glAttachShader,(GLuint,GLuint)) X(void,glLinkProgram,(GLuint)) X(void,glGetProgramiv,(GLuint,GLenum,GLint*)) X(void,glDeleteProgram,(GLuint)) X(void,glUseProgram,(GLuint)) \
 X(void,glGenVertexArrays,(GLsizei,GLuint*)) X(void,glBindVertexArray,(GLuint)) X(void,glDeleteVertexArrays,(GLsizei,const GLuint*)) X(void,glDrawArrays,(GLenum,GLint,GLsizei)) X(void,glViewport,(GLint,GLint,GLsizei,GLsizei)) \
 X(void,glGenTextures,(GLsizei,GLuint*)) X(void,glBindTexture,(GLenum,GLuint)) X(void,glTexParameteri,(GLenum,GLenum,GLint)) X(void,glTexImage2D,(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*)) X(void,glDeleteTextures,(GLsizei,const GLuint*)) \
 X(void,glGenFramebuffers,(GLsizei,GLuint*)) X(void,glBindFramebuffer,(GLenum,GLuint)) X(void,glFramebufferTexture2D,(GLenum,GLenum,GLenum,GLuint,GLint)) X(GLenum,glCheckFramebufferStatus,(GLenum)) X(void,glDeleteFramebuffers,(GLsizei,const GLuint*)) \
 X(GLint,glGetUniformLocation,(GLuint,const GLchar*)) X(void,glUniform1i,(GLint,GLint)) X(void,glUniform2f,(GLint,GLfloat,GLfloat)) X(void,glReadPixels,(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*)) X(GLenum,glGetError,(void))
#define DECLARE(ret,name,args) static ret (APIENTRY *p_##name)args;
FUNCTIONS(DECLARE)
static GLuint program,vao,input_texture,output_texture,framebuffer;static int loaded;
static GLuint compile(GLenum type,const char *source){GLuint shader=p_glCreateShader(type);p_glShaderSource(shader,1,&source,NULL);p_glCompileShader(shader);GLint ok=0;p_glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);if(!ok){p_glDeleteShader(shader);return 0;}return shader;}
static int initialize(void){
#define LOAD(ret,name,args) {retro_proc_address_t pointer=eh_gl_proc(#name);memcpy(&p_##name,&pointer,sizeof(pointer));if(!p_##name)return 0;}
 FUNCTIONS(LOAD)
 const char *vertex="#version 330 core\nout vec2 uv;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2.0-1.0,0,1);}";
 const char *fragment="#version 330 core\nin vec2 uv;out vec4 color;uniform sampler2D picture;uniform int preset;uniform vec2 size;void main(){vec3 c=texture(picture,uv).rgb;vec2 d=uv*2.0-1.0;"
 "if(preset==1){vec3 a=texture(picture,uv+vec2(1.0/size.x,0)).rgb;vec3 b=texture(picture,uv-vec2(1.0/size.x,0)).rgb;c=mix(c,(a+c+b)/3.0,0.18);c*=mix(0.80,1.0,mod(floor(gl_FragCoord.y),2.0));c*=1.0-0.16*dot(d,d);}" 
 "else if(preset==2){float x=mod(floor(gl_FragCoord.x),3.0);c*=vec3(x==0.0?1.0:0.88,x==1.0?1.0:0.88,x==2.0?1.0:0.88);c=mix(vec3(0.12,0.17,0.10),c,0.94);}" 
 "else if(preset==3){c*=vec3(1.04,1.0,0.91);c*=1.0-0.09*dot(d,d);}color=vec4(clamp(c,0.0,1.0),1.0);}";
 GLuint vs=compile(GL_VERTEX_SHADER,vertex),fs=compile(GL_FRAGMENT_SHADER,fragment);if(!vs||!fs){if(vs)p_glDeleteShader(vs);if(fs)p_glDeleteShader(fs);return 0;}
 program=p_glCreateProgram();p_glAttachShader(program,vs);p_glAttachShader(program,fs);p_glLinkProgram(program);p_glDeleteShader(vs);p_glDeleteShader(fs);GLint ok=0;p_glGetProgramiv(program,GL_LINK_STATUS,&ok);if(!ok)return 0;
 p_glGenVertexArrays(1,&vao);p_glGenTextures(1,&input_texture);p_glGenTextures(1,&output_texture);p_glGenFramebuffers(1,&framebuffer);loaded=1;return 1;
}
int eh_shader_apply(uint32_t *pixels,unsigned width,unsigned height,unsigned preset){
 if(!pixels||!width||!height||width>4096||height>4096||preset<1||preset>3)return 0;
 if(!begin()){end();close_context();return 0;}
 if(!loaded&&!initialize()){end();eh_shader_close();return 0;}
 p_glBindVertexArray(vao);p_glUseProgram(program);
 p_glBindTexture(GL_TEXTURE_2D,input_texture);p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
 p_glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_BGRA,GL_UNSIGNED_BYTE,pixels);
 p_glBindTexture(GL_TEXTURE_2D,output_texture);p_glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_BGRA,GL_UNSIGNED_BYTE,NULL);p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);p_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 p_glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);p_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,output_texture,0);
 int ok=p_glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
 if(ok){p_glViewport(0,0,width,height);p_glBindTexture(GL_TEXTURE_2D,input_texture);p_glUniform1i(p_glGetUniformLocation(program,"picture"),0);p_glUniform1i(p_glGetUniformLocation(program,"preset"),(int)preset);p_glUniform2f(p_glGetUniformLocation(program,"size"),(float)width,(float)height);p_glDrawArrays(GL_TRIANGLES,0,3);if(p_glGetError()!=GL_NO_ERROR)ok=0;else p_glReadPixels(0,0,width,height,GL_BGRA,GL_UNSIGNED_BYTE,pixels);}
 if(p_glGetError()!=GL_NO_ERROR){ok=0;}
 end();return ok;
}
void eh_shader_close(void){if(loaded&&begin()){p_glDeleteTextures(1,&input_texture);p_glDeleteTextures(1,&output_texture);p_glDeleteFramebuffers(1,&framebuffer);p_glDeleteVertexArrays(1,&vao);p_glDeleteProgram(program);end();}loaded=0;program=vao=input_texture=output_texture=framebuffer=0;close_context();}
int eh_shader_smoke(void){uint32_t pixels[16],original[16];for(int i=0;i<16;i++)original[i]=0x6080a0;memcpy(pixels,original,sizeof(pixels));int ok=eh_shader_apply(pixels,4,4,3);int changed=memcmp(pixels,original,sizeof(pixels))!=0;eh_shader_close();return ok&&changed;}
