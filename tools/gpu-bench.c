// gpu-bench: off-screen GLES rendering check + timing (surfaceless EGL).
// Draws N full-screen passes of a shader into a 3200x2136 FBO, reads back a
// pixel to verify the result, and prints the renderer and frames/s.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *vs = "attribute vec2 p; varying vec2 uv; void main(){ uv=p*0.5+0.5; gl_Position=vec4(p,0,1); }";
static const char *fs = "precision highp float; varying vec2 uv; uniform float t;"
	"void main(){ float a=0.0; for(int i=0;i<16;i++) a+=sin(uv.x*float(i)+t)*cos(uv.y*float(i));"
	" gl_FragColor=vec4(0.25, 0.5, 0.75, 1.0) + vec4(a*0.0001); }";

static GLuint sh(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	return s;
}

int main(int argc, char **argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 120, W = 3200, H = 2136;
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	if (!eglInitialize(d, NULL, NULL)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
	if (!c || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { fprintf(stderr, "context failed\n"); return 1; }
	printf("renderer: %s\n", glGetString(GL_RENDERER));

	GLuint tex, fbo;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	GLuint pr = glCreateProgram();
	glAttachShader(pr, sh(GL_VERTEX_SHADER, vs));
	glAttachShader(pr, sh(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(pr, 0, "p");
	glLinkProgram(pr);
	glUseProgram(pr);
	GLint tu = glGetUniformLocation(pr, "t");
	float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, W, H);

	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (int i = 0; i < frames; i++) {
		glUniform1f(tu, i * 0.01f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	unsigned char px[4];
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);   // forces completion
	clock_gettime(CLOCK_MONOTONIC, &b);
	double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
	int ok = abs(px[0] - 64) <= 2 && abs(px[1] - 128) <= 2 && abs(px[2] - 191) <= 2;
	printf("%d full-screen passes at %dx%d in %.2f s = %.1f fps; centre pixel %u,%u,%u (%s)\n",
	       frames, W, H, s, frames / s, px[0], px[1], px[2], ok ? "correct" : "WRONG");
	return !ok;
}
