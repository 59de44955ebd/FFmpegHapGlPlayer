#pragma warning(disable : 4267)

#include "HAPAvFormatOpenGLRenderer.h"
#include "hap/hap.h"
#include <fstream>
#include <sstream>

// Multi-threaded decode function is platform specific
#if defined(__APPLE__)
    #include <dispatch/dispatch.h>
#else
    #include <ppl.h>
#endif

void HapMTDecode(HapDecodeWorkFunction function, void *info, unsigned int count, void * /*info*/)
{
    #if defined(__APPLE__)
        dispatch_apply(count, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^(size_t index) {
            function(info, (unsigned int)index);
        });
    #else
        concurrency::parallel_for((unsigned int)0, count, [&](unsigned int i) {
            function(info, i);
        });
    #endif
}

// Initialization of HAPAvFormatOpenGLRenderer is done in constructor, including
// creating opengl textures, which mean we should already be in the GL context
// we'll use when drawing frames
HAPAvFormatOpenGLRenderer::HAPAvFormatOpenGLRenderer(AVCodecContext* codecCtx)
{
    #define FFALIGN(x, a) (((x)+(a)-1)&~((a)-1))
    #define TEXTURE_BLOCK_W 4
    #define TEXTURE_BLOCK_H 4

    m_textureWidth = codecCtx->width;
    m_textureHeight = codecCtx->height;
    // Encoded texture is 4 bytes aligned
    m_codedWidth = FFALIGN(m_textureWidth,TEXTURE_BLOCK_W);
    m_codedHeight = FFALIGN(m_textureHeight,TEXTURE_BLOCK_H);
    m_textureCount = 1;
    unsigned int outputBufferTextureFormats[2];
    switch (codecCtx->codec_tag) {
    	
    case MKTAG('H','a','p','1'): // Hap
        outputBufferTextureFormats[0] = HapTextureFormat_RGB_DXT1;
        m_glInternalFormat[0] = HapTextureFormat_RGB_DXT1;
        break;
    case MKTAG('H','a','p','5'): // Hap Alpha
        outputBufferTextureFormats[0] = HapTextureFormat_RGBA_DXT5;
        m_glInternalFormat[0] = HapTextureFormat_RGBA_DXT5;
        break;
    case MKTAG('H','a','p','Y'): // Hap Q
        outputBufferTextureFormats[0] = HapTextureFormat_YCoCg_DXT5;
        m_glInternalFormat[0] = HapTextureFormat_RGBA_DXT5;
        break;
    case MKTAG('H','a','p','A'): // Hap Alpha Only
        outputBufferTextureFormats[0] = HapTextureFormat_A_RGTC1;
        m_glInternalFormat[0] = HapTextureFormat_A_RGTC1;
        break;
    case MKTAG('H','a','p','M'):
        m_textureCount = 2;
        outputBufferTextureFormats[0] = HapTextureFormat_YCoCg_DXT5;
        outputBufferTextureFormats[1] = HapTextureFormat_A_RGTC1;
        m_glInternalFormat[0] = HapTextureFormat_RGBA_DXT5;
        m_glInternalFormat[1] = HapTextureFormat_A_RGTC1;
        break;
    default:
        assert(false);
        throw std::runtime_error("Unhandled HAP codec tab");
    }

    for (int textureId = 0; textureId < m_textureCount; textureId++) {
        unsigned int bitsPerPixel = 0;
        switch (outputBufferTextureFormats[textureId]) {
            case HapTextureFormat_RGB_DXT1:
                bitsPerPixel = 4;
                break;
            case HapTextureFormat_RGBA_DXT5:
            case HapTextureFormat_YCoCg_DXT5:
                bitsPerPixel = 8;
                break;
            case HapTextureFormat_A_RGTC1:
                bitsPerPixel = 4;
                break;
            default:
                throw std::runtime_error("Invalid texture format");
        }

        size_t bytesPerRow = (m_codedWidth * bitsPerPixel) / 8;

        m_outputBufferSize[textureId] = bytesPerRow * m_codedHeight;
        m_outputBuffers[textureId] = malloc(m_outputBufferSize[textureId]);
        if (m_outputBuffers[textureId] == nullptr) {
            throw std::runtime_error("Failed allocating memory for decompressed texture");
        }

        glGenTextures(1, &m_outputTextures[textureId]); // crash

        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, m_outputTextures[textureId]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        #ifdef __APPLE__
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_STORAGE_HINT_APPLE , GL_STORAGE_SHARED_APPLE);
        #endif

        glTexImage2D(GL_TEXTURE_2D, 0, m_glInternalFormat[textureId], m_codedWidth, m_codedHeight, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    }

    createShaderProgram(codecCtx->codec_tag);
}

// This function will decode the AVPacket into memory buffers using HapDecode
// and will then upload the binary result as an OpenGL texture of the correct type
// It then renders a quad into the current framebuffer using the appropriate shader program
void HAPAvFormatOpenGLRenderer::renderFrame(AVPacket* packet) {
    // Update textures
    for (int textureId = 0; textureId < m_textureCount; textureId++) {
        size_t outputBufferDecodedSize;
        unsigned int outputBufferTextureFormat;
        unsigned int res = HapDecode(
			packet->data,
			packet->size,
			0,
			HapMTDecode,
			nullptr,
			m_outputBuffers[textureId],
			m_outputBufferSize[textureId],
			(unsigned long *)&outputBufferDecodedSize,
			&outputBufferTextureFormat
		);
        if (res != HapResult_No_Error) {
            throw std::runtime_error("Failed to decode HAP texture");
        }

        #ifdef __APPLE__
            glTextureRangeAPPLE(GL_TEXTURE_2D, m_outputBufferSize[textureId], m_outputBuffers[textureId]);
            glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
        #endif

        glActiveTexture(GL_TEXTURE0 + textureId);
        glBindTexture(GL_TEXTURE_2D, m_outputTextures[textureId]);
        glCompressedTexSubImage2D(GL_TEXTURE_2D,
            0,
            0,
            0,
            m_codedWidth,
            m_codedHeight,
            m_glInternalFormat[textureId],
            m_outputBufferSize[textureId],
            m_outputBuffers[textureId]);
    }

    // Render with old school OpenGL 2 code to avoid adding complexity here
    // The objective is just to understand the process
    // (in a real application don't use glBegin/glEnd, use VBOs)
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glUseProgram(m_shaderProgram);

    glBegin(GL_QUADS);
    glTexCoord2f(0, 1); glVertex2f(-1,-1);
    glTexCoord2f(0, 0); glVertex2f(-1,1);
    glTexCoord2f(1, 0); glVertex2f(1,1);
    glTexCoord2f(1, 1); glVertex2f(1,-1);
    glEnd();

    glUseProgram(0);
}

// Compiles a shader program given its type and its path relative to executable
GLuint compileShaderProgram(GLenum shaderType, std::string relativeFilePath)
{

    std::ifstream t(relativeFilePath);
    std::stringstream buffer;
    buffer << t.rdbuf();
    std::string fileContent = buffer.str();
    const GLcharARB * cFileContent = (const GLcharARB *)fileContent.c_str();
    GLuint shader = glCreateShader(shaderType);
    glShaderSource(shader, 1, &cFileContent, nullptr);
    glCompileShader(shader);

    // Verify compilation
    GLint shaderCompiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &shaderCompiled);
    if (shaderCompiled == 0) {
        throw std::runtime_error("Error compiling vertex shader");
    }

    return shader;
}

void HAPAvFormatOpenGLRenderer::createShaderProgram(unsigned int codecTag)
{
    // Get vertex and fragment shader files to use
    std::string vertexFilePath = "shaders/Default.vert";
    std::string fragmentFilePath = "shaders/Default.frag";
    
    switch (codecTag) {
        case MKTAG('H','a','p','1'): // Hap
        case MKTAG('H','a','p','5'): // Hap Alpha
            break;
        case MKTAG('H','a','p','A'): // Hap Alpha Only
            // This file does only contain alpha (no RGB) so we should
            // handle it with a specific shader, but this a codec to use
            // only in tricky situations and we'll ignore it for this example
            break;
            
        case MKTAG('H','a','p','Y'): // Hap Q
            // Single texture with HapTextureFormat_YCoCg_DXT5;
            fragmentFilePath = "shaders/ScaledCoCgYToRGBA.frag";
            break;
            
        case MKTAG('H','a','p','M'):
            // Two textures: HapTextureFormat_YCoCg_DXT5 & HapTextureFormat_A_RGTC1;
            fragmentFilePath = "shaders/ScaledCoCgYPlusAToRGBA.frag";
            break;
        default:
            assert(false);
            throw std::runtime_error("Unhandled HAP codec tab");
    }

    GLuint vertexShaderObject = compileShaderProgram(GL_VERTEX_SHADER,vertexFilePath);
    GLuint fragmentShaderObject = compileShaderProgram(GL_FRAGMENT_SHADER,fragmentFilePath);

    // Build shader program
    m_shaderProgram = glCreateProgram();
    glAttachShader(m_shaderProgram, vertexShaderObject);
    glAttachShader(m_shaderProgram, fragmentShaderObject);
    glLinkProgram(m_shaderProgram);

    // Verify link
    GLint programLinked = 0;
    glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &programLinked);
    if(programLinked == 0 ) {
        throw std::runtime_error("Error compiling fragment shader");
    } else {
        glUseProgram(m_shaderProgram);
        GLint samplerLoc = -1;
        samplerLoc = glGetUniformLocation(m_shaderProgram, "cocgsy_src");
        if (samplerLoc >= 0) {
            glUniform1i(samplerLoc,0);
        }
        glUseProgram(0);
    }
}
