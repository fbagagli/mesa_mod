/*
 * [ARCHITECTURAL OVERVIEW]
 * Component:        Single-file OpenGL ES 2.0 Renderer
 * Window System:    X11 (Xlib)
 * Glue Layer:       EGL 1.4+
 * Driver Model:     Mesa (supports native hardware or Virgl/Virtio-GPU)
 *
 * [COMPILE INSTRUCTION]
 * Default (Direct Rendering / DRI3):
 * gcc main_xlib_and_egl_detailed.c -o triangle_dri -lX11 -lEGL -lGLESv2
 *
 * Readback Fallback (Bypasses DRI3, uses CPU copy):
 * gcc main_xlib_and_egl_detailed.c -o triangle_readback -DENABLE_READBACK -lX11 -lEGL -lGLESv2
 */

/* * [MACRO CONTROL: ENABLE_READBACK]
 * If defined, the application switches from the Standard Presentation Path (eglSwapBuffers)
 * to a Manual Presentation Path (FBO -> glReadPixels -> XPutImage).
 * * USE CASE:
 * This is specifically designed for Virtualized Environments (e.g., QEMU/Virgl with DRI_PRIME)
 * where the DRI3/DMABUF buffer sharing mechanism between the Guest GPU Driver and
 * the Guest X Server is broken (Error: dri3_alloc_render_buffer failed).
 */
// #define ENABLE_READBACK

#include <X11/Xlib.h>
#include <X11/Xutil.h> // Needed for XPutImage/XDestroyImage utilities
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ENABLE_READBACK
#ifdef ENABLE_READBACK
#include <EGL/eglext.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#define WIN_WIDTH  800
#define WIN_HEIGHT 600

/* * --- DATA LOCATION: SYSTEM RAM (Process .rodata) ---
 * These strings exist in the application's virtual address space (System Memory).
 * * [VIRTUALIZATION NOTE]
 * When these are passed to the driver, the Guest Driver copies them into a
 * command buffer. In a VM (Virgl), these strings are serialized and sent
 * over the VIRTIO bus to the Host, where the Host GPU driver actually compiles them.
 */
const char *vertex_shader_src =
    "attribute vec4 position;    \n"
    "void main()                 \n"
    "{                           \n"
    "   gl_Position = position;  \n"
    "}                           \n";

const char *fragment_shader_src =
    "precision mediump float;    \n"
    "void main()                 \n"
    "{                           \n"
    "   gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); \n" // Red Color
    "}                           \n";

/*
 * [HELPER] load_shader
 * --------------------
 * Orchestrates the compilation pipeline.
 * 1. Allocates shader object in Driver.
 * 2. Copies source text from RAM to Driver.
 * 3. Driver compiles text to GPU microcode.
 *
 * [MEMORY FLOW]
 * User RAM (src) -> Driver Heap (Copy) -> GPU Instruction Cache (Binary)
 */
GLuint load_shader(const char *src, GLenum type) {
    /* The driver (Mesa) goes to its own private memory area (in System RAM)
     * and creates a struct.
     * RETURN VALUE: A Handle (e.g., 1).
     * You don't get a pointer to the actual memory. You just get an ID.
     * Whenever you say "Do something to Shader #1," the driver looks up
     * that internal structure.
     */
    GLuint shader = glCreateShader(type);

    // DATA COPY: Source string is copied from User RAM to Driver-managed memory.
    glShaderSource(shader, 1, &src, NULL);

    // COMPUTATION: Driver invokes the compiler (LLVM/ACO in Mesa).
    // Native: Translated to ISA (Instruction Set Architecture).
    // Virgl:  Translated to TGSI/NIR (Intermediate Representation) and sent to Host.
    glCompileShader(shader);

    return shader;
}

int main() {
    /* * --- STAGE 1: X11 PLUMBING (Window System) ---
     * SCOPE: CPU / System RAM
     * Connect to the Linux Window System.
     */
    Display *x_display = XOpenDisplay(NULL);
    if (!x_display) {
        fprintf(stderr, "Error: Unable to open X Display\n");
        return 1;
    }

    Window root = DefaultRootWindow(x_display);
    XSetWindowAttributes swa;
    swa.event_mask = ExposureMask | KeyPressMask;

    // ALLOCATION: Creates the window structure in the X Server (System RAM).
    Window win = XCreateWindow(x_display, root, 0, 0, WIN_WIDTH, WIN_HEIGHT, 0,
                               CopyFromParent, InputOutput,
                               CopyFromParent, CWEventMask, &swa);

    XMapWindow(x_display, win);
    XStoreName(x_display, win, "Mesa/Virgl Analysis");

#ifdef ENABLE_READBACK
    /* [MANUAL PRESENTATION SETUP]
     * We need a Graphics Context (GC) to use XPutImage.
     * In the standard path, EGL handles this internally. Here we do it manually.
     */
    GC gc = XCreateGC(x_display, win, 0, NULL);
#endif

    /* --- STAGE 2: EGL PLUMBING (The Bridge) ---
     * SCOPE: Bridge between X11 and the GPU Driver.
     */

    // 1. Get the EGL handle
#ifdef ENABLE_READBACK
    /* [DRI3 ERROR FIX / DECOUPLING]
     * Issue: Passing x_display to eglGetDisplay forces Mesa to initialize the X11/DRI3
     * backend. In broken environments, this triggers 'dri3_alloc_render_buffer' errors
     * during eglInitialize, even if we don't use Window Surfaces.
     */
    PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT =
        (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    if (!eglQueryDevicesEXT || !eglGetPlatformDisplayEXT) {
        fprintf(stderr, "Error: Required EGL device extensions missing.\n");
        return 1;
    }

    // 1. Query available EGL devices
    EGLint num_devices = 0;
    eglQueryDevicesEXT(0, NULL, &num_devices);
    if (num_devices == 0) {
        fprintf(stderr, "Error: No EGL devices found.\n");
        return 1;
    }

    EGLDeviceEXT* devices = (EGLDeviceEXT*)malloc(sizeof(EGLDeviceEXT) * num_devices);
    eglQueryDevicesEXT(num_devices, devices, &num_devices);

    // 2. Select the correct device (Targeting the Intel GPU)
    // Note: In a robust application, you would query the device properties
    // (e.g., via EGL_DRM_DEVICE_FILE_EXT) to match the exact render node path
    // instead of hardcoding an index. For this example, we assume device index 1
    // is the Intel GPU, or you must iterate to find the one matching /dev/dri/renderD129.

    EGLDeviceEXT target_device = EGL_NO_DEVICE_EXT;

    // --- Simplistic Device Selection ---
    // You may need to change '1' to '0' depending on how Mesa enumerates them
    // on your specific QEMU guest configuration.
    if (num_devices > 1) {
         target_device = devices[1]; // Attempt to grab the secondary device
    } else {
         target_device = devices[0];
    }

    // 3. Initialize display with the explicit device
    EGLDisplay egl_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, target_device, NULL);
    free(devices);

    if (egl_display == EGL_NO_DISPLAY) {
         fprintf(stderr, "Error: Failed to get platform display for device.\n");
         return 1;
    }
#else
    /* [STANDARD PATH]
     * We strictly need the X11 display connection so EGL can negotiate
     * DRI3 buffer sharing with the X Server.
     */
    EGLDisplay egl_display = eglGetDisplay((EGLNativeDisplayType)x_display);
#endif

    if (egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Error: EGL Get Display failed\n");
        return 1;
    }

    // 2. Initialize the driver
    // Mesa loads here. It detects hardware (/dev/dri/card0) or software renderers.
    // [VIRTUALIZATION] If in a VM, this initializes the virtio-gpu kernel driver interaction.
    if (!eglInitialize(egl_display, NULL, NULL)) {
        fprintf(stderr, "Error: EGL Initialize failed\n");
        return 1;
    }

    // 3. Negotiate Config
    EGLConfig ecfg;
    EGLint num_config;

#ifdef ENABLE_READBACK
    /* [PATH B: OFFSCREEN / PBUFFER CONFIG]
     * We explicitly request EGL_PBUFFER_BIT.
     * We DO NOT request EGL_WINDOW_BIT.
     * Since we used EGL_DEFAULT_DISPLAY, EGL doesn't even know about X11 Visuals here.
     * It just selects a config compatible with the GPU hardware.
     */
    EGLint attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
#else
    /* [PATH A: STANDARD WINDOW CONFIG]
     * Implicitly requests EGL_WINDOW_BIT.
     * The driver assumes we will create a surface visible on the X server.
     */
    EGLint attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
#endif

    if (!eglChooseConfig(egl_display, attr, &ecfg, 1, &num_config)) {
        fprintf(stderr, "Error: Failed to choose config\n");
        return 1;
    }

    // 4. Create the GPU Context (State Machine)
    /* * [MEMORY DOMAIN: USERSPACE RAM]
     * The "Context" is the data structure that holds the current state of the
     * OpenGL machine (e.g., "Blending is ON", "Texture #5 is bound").
     * * 1. User-Mode Driver (Mesa): Allocates a struct (gl_context) in YOUR
     * application's heap (System RAM). This is where the API state lives.
     * * 2. Kernel-Mode Driver: Receives an ioctl to register a "Hardware Context"
     * (a scheduling handle), but does NOT store the detailed API state.
     * * 3. [VIRGL]: Sends 'VIRTIO_GPU_CMD_CTX_CREATE' to the Host.
     * The Host creates a REAL OpenGL context in Host RAM to shadow this one.
     */
    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_display, ecfg, EGL_NO_CONTEXT, ctxattr);

    // 5. Create Surface and Make Current
    EGLSurface egl_surf;

#ifdef ENABLE_READBACK
    /* [PATH B: DUMMY PBUFFER SURFACE]
     * We create a tiny 1x1 offscreen surface.
     * This works perfectly with EGL_DEFAULT_DISPLAY.
     */
    EGLint pbuffer_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    egl_surf = eglCreatePbufferSurface(egl_display, ecfg, pbuffer_attribs);
#else
    /* [PATH A: NATIVE WINDOW SURFACE]
     * Connects the EGL Context to the X11 Window 'win'.
     * This is where `dri3_alloc_render_buffer` typically fails on broken setups
     * because it tries to allocate a shareable GEM buffer.
     */
    /* * [CRITICAL CONCEPT: THE BUFFERING STRATEGY]
     * This step allocates the actual pixel storage (The Back Buffer).
     *
     * STRATEGY: Double Buffering
     * --------------------------
     * 1. FRONT BUFFER: What the user currently sees on screen. Owned by X Server (or Compositor).
     * 2. BACK BUFFER:  Where the GPU is currently drawing. Owned by EGL/Mesa.
     *
     * [ALLOCATION LOCATION - WHERE ARE THE PIXELS?]
     *
     * A. NATIVE LINUX (Discrete/Integrated GPU):
     * - The driver (Mesa) uses the Kernel DRM subsystem (Direct Rendering Manager).
     * - Allocation: Video RAM (VRAM) for Discrete GPUs, or System RAM (GART) for Integrated.
     * - Handle: A "GEM Handle" or "DMABUF" is created.
     * - The X Server and Client share this buffer via "DRI3" (Direct Rendering Infrastructure 3)
     * by passing file descriptors, preventing memory copies.
     *
     * B. VIRGL (Virtual Machine):
     * - GUEST SIDE: The Guest Kernel allocates a "Dummy" resource in Guest RAM.
     * It serves as a placeholder ID.
     * - HOST SIDE: The Host (QEMU/Virglrenderer) receives a VIRTIO command to create
     * a real OpenGL Texture/Renderbuffer on the Host GPU.
     * - REALITY: The actual pixels live in the HOST GPU VRAM. The Guest only holds
     * a reference to them.
     */
    egl_surf = eglCreateWindowSurface(egl_display, ecfg, (EGLNativeWindowType)win, NULL);
#endif

    // 6. Make Current
    // "Binds" the context to this thread. All future GL calls go here.
    eglMakeCurrent(egl_display, egl_surf, egl_surf, egl_ctx);

    /* * --- STAGE 3: GPU SETUP (The Pipeline) ---
     * Creating the GPU "Program" (Compiling Shaders)
     * * [CONCEPT] The GPU Pipeline
     * Think of the GPU not as a single processor, but as a factory assembly line.
     * The Vertex Shader and Fragment Shader are two programmable stations on this line.
     * You write the code for these stations, and the GPU runs that code for
     * *every single point and pixel you draw*.
     * * FLOW: Input Data (RAM) -> Vertex Shader -> Rasterizer (Fixed) -> Fragment Shader -> Screen
     */

    /* * --- STAGE 3: RESOURCE SETUP --- */

#ifdef ENABLE_READBACK
    /* [PATH B: FBO & XIMAGE SETUP]
     * Since we don't have a Window Surface to render into, we must manually create
     * a Framebuffer Object (FBO) backed by a Texture.
     */

    // 1. Create Texture (The VRAM Storage)
    GLuint fbo_texture;
    glGenTextures(1, &fbo_texture);
    glBindTexture(GL_TEXTURE_2D, fbo_texture);
    // Allocate VRAM (or Guest RAM acting as VRAM)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, WIN_WIDTH, WIN_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // 2. Create FBO (The Metadata)
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Error: FBO incomplete\n");
        return 1;
    }

    // 3. Allocate System RAM for Readback
    // This buffer lives in CPU memory. We will copy from GPU -> Here -> X Server.
    size_t buffer_size = WIN_WIDTH * WIN_HEIGHT * 4;
    unsigned char* client_pixels = (unsigned char*)malloc(buffer_size);
    if (!client_pixels) return 1;

    /* Create XImage wrapper around our CPU buffer.
     * XCreateImage tells Xlib: "I have some bytes at 'client_pixels', treat them as an image."
     * Depth: 24 (Color), Format: ZPixmap (Pixel array), BitmapPad: 32 (Align lines)
     */
    XImage *ximage = XCreateImage(x_display, DefaultVisual(x_display, 0), 24, ZPixmap, 0,
                                  (char*)client_pixels, WIN_WIDTH, WIN_HEIGHT, 32, 0);
#endif

    /* * --- STAGE 4: GPU PIPELINE SETUP (Shared) --- */

    GLuint vs = load_shader(vertex_shader_src, GL_VERTEX_SHADER);
    GLuint fs = load_shader(fragment_shader_src, GL_FRAGMENT_SHADER);

    // Create the Program container
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);

    // LINKING
    // Native: Combines ISA into a final executable microcode block.
    // Virgl:  Validation. The Host driver performs the real linking of binaries.
    glLinkProgram(program);
    glUseProgram(program);

    /* * --- STAGE 4: DATA PREPARATION ---
     * SCOPE: Data preparation in System RAM
     */

    // DATA LOCATION: SYSTEM RAM (Stack)
    // This array sits in the CPU Stack. It is NOT on the GPU yet.
    // The GPU cannot read this memory directly.
    GLfloat vertices[] = {
         0.0f,  0.5f, 0.0f, // Top
        -0.5f, -0.5f, 0.0f, // Bottom Left
         0.5f, -0.5f, 0.0f  // Bottom Right
    };

    // Get the "handle" ID for the "position" variable in the shader
    GLint pos_attr_loc = glGetAttribLocation(program, "position");

    // CRITICAL: POINTER SETUP
    // This function does NOT copy data.
    // It tells the driver: "When you need to draw, look at address &vertices in System RAM".
    // [PERFORMANCE WARNING] Using raw pointers (Client-Side Arrays) forces the driver
    // to copy data every frame. Use VBOs (glGenBuffers) for VRAM storage.
    glVertexAttribPointer(pos_attr_loc, 3, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(pos_attr_loc);

    printf("GL Renderer: %s\n", glGetString(GL_RENDERER));
    printf("GL Version:  %s\n", glGetString(GL_VERSION));
#ifdef ENABLE_READBACK
    printf("Mode:        READBACK (FBO -> glReadPixels -> XPutImage)\n");
#else
    printf("Mode:        STANDARD (DRI3/SwapBuffers)\n");
#endif

    /* * --- STAGE 5: RENDER LOOP --- */
    int running = 1;
    XEvent xev;

    while(running) {
        // Check X11 events (Keyboard, Close window, etc)
        while(XPending(x_display)) {
            XNextEvent(x_display, &xev);
            if(xev.type == KeyPress) running = 0;
        }

        /* 1. BIND TARGET */
#ifdef ENABLE_READBACK
        // Render to our offscreen texture
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, WIN_WIDTH, WIN_HEIGHT);
#else
        // Render to the Window Surface (Back Buffer)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        // Viewport is usually automatic, but safe to set
        glViewport(0, 0, WIN_WIDTH, WIN_HEIGHT);
#endif

        /* 2. DRAW */
        glClearColor(0.2f, 0.8f, 0.2f, 1.0f); // Dark Grey Background
        glClear(GL_COLOR_BUFFER_BIT);

        // 2. DRAW (The Critical Data Movement)
        // Because we used a RAM pointer (glVertexAttribPointer), the driver ACTS here.
        //
        // [NATIVE EXECUTION]
        // 1. Driver allocates a temporary "Staging Buffer" in GART (CPU/GPU shared RAM).
        // 2. Driver `memcpy` data from Stack `vertices` -> Staging Buffer.
        // 3. GPU is told to fetch data from Staging Buffer.
        //
        // [VIRGL/VIRTUAL EXECUTION]
        // 1. Guest Driver reads `vertices` from Guest RAM.
        // 2. Encodes data into a VIRTIO command buffer.
        // 3. "Kicks" the Virtio Queue.
        // 4. QEMU/Host reads queue, copies data to Host Driver, and executes draw.
        glDrawArrays(GL_TRIANGLES, 0, 3);

        /* 3. PRESENTATION */
#ifdef ENABLE_READBACK
        /* [PATH B: MANUAL READBACK & UPLOAD]
         * * 1. SYNCHRONIZATION: glFinish()
         * Force the CPU to wait until the GPU has finished drawing the triangle
         * into the FBO texture.
         */
        glFinish();

        /* 2. READBACK (GPU VRAM -> CPU RAM)
         * Transfer data over PCIe (or Virtual Bus).
         * Format: RGBA (Standard OpenGL)
         */
        glReadPixels(0, 0, WIN_WIDTH, WIN_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, client_pixels);

        /* 3. FLIP & SWIZZLE (RGBA -> BGRA, Bottom-Left -> Top-Left)
         * - OpenGL Origin: Bottom-Left.
         * - X11 Origin: Top-Left.
         * - OpenGL Format: RGBA.
         * - X11 Format: BGRA (Little Endian).
         */

        int stride = WIN_WIDTH * 4;
        unsigned char row_buf[WIN_WIDTH * 4]; // Stack allocation for temporary row

        // PHASE A: VERTICAL FLIP
        // We swap the top rows with the bottom rows to correct the orientation.
        for (int y = 0; y < WIN_HEIGHT / 2; ++y) {
            unsigned char* top_ptr = client_pixels + (y * stride);
            unsigned char* bot_ptr = client_pixels + ((WIN_HEIGHT - 1 - y) * stride);

            // Swap rows using temporary buffer
            memcpy(row_buf, top_ptr, stride);
            memcpy(top_ptr, bot_ptr, stride);
            memcpy(bot_ptr, row_buf, stride);
        }

        // PHASE B: COLOR SWIZZLE
        // X11 expects BGRA, but OpenGL gave us RGBA. We must swap R and B.
        for (int i = 0; i < WIN_WIDTH * WIN_HEIGHT * 4; i += 4) {
            unsigned char temp = client_pixels[i];     // R
            client_pixels[i] = client_pixels[i+2];     // B -> R
            client_pixels[i+2] = temp;                 // R -> B
        }

        /* 5. UPLOAD TO X SERVER (CPU RAM -> X Socket -> GPU Front Buffer)
         * We push the raw pixels to the X Server.
         * If Local: Memory Copy.
         * If Remote: Network transmission.
         */
        XPutImage(x_display, win, gc, ximage, 0, 0, 0, 0, WIN_WIDTH, WIN_HEIGHT);

#else
        /* [PATH A: ZERO-COPY SWAP]
         * The standard mechanism. Handles buffer exchange handles with the X Server.
         */
        eglSwapBuffers(egl_display, egl_surf);
#endif
    }

    // Cleanup
#ifdef ENABLE_READBACK
    // XDestroyImage frees the data pointer (client_pixels) too!
    XDestroyImage(ximage);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &fbo_texture);
    XFreeGC(x_display, gc);
#endif

    eglDestroyContext(egl_display, egl_ctx);
    eglDestroySurface(egl_display, egl_surf);
    eglTerminate(egl_display);
    XDestroyWindow(x_display, win);
    XCloseDisplay(x_display);

    return 0;
}

/* Notes */
/* [1] Compositing (Windowed Mode)
Context: Running inside a window manager (GNOME, KDE, i3) on X11.
- Mechanism (Compositing - DRI3):
    1) Your application owns the Back Buffer (a GEM/DMABUF object).
    2) eglSwapBuffers notifies the X Server/Compositor that a new buffer is ready.
    3) The Exchange: The Compositor takes ownership of your Back Buffer handle
       and uses it as a texture to render the desktop. It does not scan this
       buffer out directly to the monitor; it draws it onto the Desktop's own buffer.

- Mechanism (Legacy X11 - DRI2/Copy):
    1) The X Server owns the window's destination surface.
    2) eglSwapBuffers triggers a Block Transfer (Blit).
    3) The Copy: The GPU copies pixels from your Back Buffer area to the
       Front Buffer area in VRAM. This is slower than flipping.
*/

