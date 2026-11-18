#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <string>

#include "graphics.h"
#include "log.h"

/* Jeder Framebuffer beginnt auf einer 2-MiB-Grenze. */
#define DISPLAY_ALIGNMENT 0x200000

Scene2D::Scene2D(int w, int h, int pixelDepth)
{
	this->width = w;
	this->height = h;
	this->depth = pixelDepth;
	
	this->frameBufferSize = this->width * this->height * this->depth;
	
	// Alles auf "nicht initialisiert" setzen, damit cleanupInit() nach einem
	// Fehlschlag nichts Zufaelliges freigibt.
	this->video = -1;
	this->flipQueue = 0;
	this->videoMem = NULL;
	this->videoMemSP = 0;
	this->directMemOff = 0;
	this->directMemAllocationSize = 0;
	this->frameBuffers = NULL;
	this->activeFrameBufferIdx = 0;
	this->initStage = SCENE_INIT_NONE;
	this->initCode = 0;
}

bool Scene2D::Init(size_t memSize, int numFrameBuffers)
{
	this->initStage = SCENE_INIT_VIDEO_OPEN;
	this->initCode = 0;
	
	this->video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
	this->videoMem = NULL;
	this->initCode = this->video;

	if (this->video < 0)
	{
		cleanupInit();
		return false;
	}
	
#ifdef GRAPHICS_USES_FONT
	int rc;

	// Load freetype
	rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_FREETYPE_OL);

	if (rc < 0)
	{
		DEBUGLOG << "Failed to load freetype: " << std::string(strerror(errno));
		return false;
	}

	// Initialize freetype
	rc = FT_Init_FreeType(&this->ftLib);

	if (rc != 0)
	{
		DEBUGLOG << "Failed to initialize freetype: " << std::string(strerror(errno));
		return false;
	}
#endif
	
	if(!initFlipQueue())
	{
		cleanupInit();
		return false;
	}
	
	if(!allocateVideoMem(memSize, 0x200000))
	{
		cleanupInit();
		return false;
	}
	
	if(!allocateFrameBuffers(numFrameBuffers))
	{
		cleanupInit();
		return false;
	}
	
	this->initStage = SCENE_INIT_FLIP_RATE;
	int result = sceVideoOutSetFlipRate(this->video, 0);
	this->initCode = result;

	if (result < 0)
	{
		cleanupInit();
		return false;
	}

	this->initStage = SCENE_INIT_NONE;
	this->initCode = 0;
	return true;
}

bool Scene2D::initFlipQueue()
{
	this->initStage = SCENE_INIT_FLIP_QUEUE;
	int rc = sceKernelCreateEqueue(&flipQueue, "homebrew flip queue");
	this->initCode = rc;
	
	if(rc < 0)
		return false;
	
	this->initStage = SCENE_INIT_FLIP_EVENT;
	rc = sceVideoOutAddFlipEvent(flipQueue, this->video, 0);
	this->initCode = rc;
	
	return rc == 0;
}

bool Scene2D::allocateFrameBuffers(int num)
{
	this->initStage = SCENE_INIT_FRAME_BUFFERS;
	this->initCode = 0;

	// Allocate frame buffers array
	this->frameBuffers = new char*[num];
	
	// Set the display buffers
	for(int i = 0; i < num; i++)
	{
		this->frameBuffers[i] = this->allocateDisplayMem(frameBufferSize);
		if (this->frameBuffers[i] == 0)
		{
			return false;
		}
	}

	// Set SRGB pixel format
	sceVideoOutSetBufferAttribute(&this->attr, 0x80000000, 1, 0, this->width, this->height, this->width);
	
	// Register the buffers to the video handle
	this->initStage = SCENE_INIT_REGISTER_BUFFERS;
	int rc = sceVideoOutRegisterBuffers(this->video, 0, (void **)this->frameBuffers, num, &this->attr);
	this->initCode = rc;
	
	return rc == 0;
}

char *Scene2D::allocateDisplayMem(size_t size)
{
	// Jeder Puffer beginnt auf einer 2-MiB-Grenze statt direkt hinter dem
	// vorherigen. Bei 8.294.400 Byte Puffergroesse sind das je 8 MiB, also
	// zusammen exakt 16 MiB - der angeforderte Direktspeicher reicht damit
	// ohne Reserve, deshalb die Bereichspruefung.
	uintptr_t base = (uintptr_t)this->videoMem;
	uintptr_t aligned = (this->videoMemSP + (DISPLAY_ALIGNMENT - 1)) & ~(uintptr_t)(DISPLAY_ALIGNMENT - 1);

	if (this->videoMem == 0 || this->directMemAllocationSize == 0 ||
		aligned < base || aligned + size > base + this->directMemAllocationSize)
	{
		return 0;
	}

	this->videoMemSP = aligned + size;
	return (char *)aligned;
}

bool Scene2D::allocateVideoMem(size_t size, int alignment)
{
	int rc;
	
	// Align the allocation size
	this->directMemAllocationSize = (size + alignment - 1) / alignment * alignment;
	
	// Allocate memory for display buffer
	this->initStage = SCENE_INIT_DIRECT_MEMORY;
	rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), this->directMemAllocationSize, alignment, 3, &this->directMemOff);
	this->initCode = rc;
	
	if(rc < 0)
	{
		this->directMemAllocationSize = 0;
		return false;
	}
	
	// Map the direct memory
	this->initStage = SCENE_INIT_DIRECT_MAP;
	rc = sceKernelMapDirectMemory(&this->videoMem, this->directMemAllocationSize, 0x33, 0, this->directMemOff, alignment);
	this->initCode = rc;
	
	if(rc < 0)
	{
		sceKernelReleaseDirectMemory(this->directMemOff, this->directMemAllocationSize);
		
		this->directMemOff = 0;
		this->directMemAllocationSize = 0;
		
		return false;
	}
	
	// Set the stack pointer to the beginning of the buffer
	this->videoMemSP = (uintptr_t)this->videoMem;
	return true;
}

/*
 * Nimmt eine teilweise oder vollstaendig gelungene Initialisierung zurueck.
 * Wichtig nach einem Fehlschlag: ohne das bliebe der Video-Handle offen und
 * jeder weitere sceVideoOutOpen-Versuch liefert nur einen Folgefehler.
 */
void Scene2D::cleanupInit()
{
	if (this->frameBuffers != NULL)
	{
		delete[] this->frameBuffers;
		this->frameBuffers = NULL;
	}
	
	if (this->videoMem != NULL && this->directMemAllocationSize != 0)
	{
		sceKernelMunmap(this->videoMem, this->directMemAllocationSize);
		this->videoMem = NULL;
	}
	
	if (this->directMemAllocationSize != 0)
	{
		sceKernelReleaseDirectMemory(this->directMemOff, this->directMemAllocationSize);
		this->directMemOff = 0;
		this->directMemAllocationSize = 0;
	}
	
	if (this->flipQueue != 0)
	{
		sceKernelDeleteEqueue(this->flipQueue);
		this->flipQueue = 0;
	}
	
	if (this->video >= 0)
	{
		sceVideoOutClose(this->video);
		this->video = -1;
	}
	
	this->videoMemSP = 0;
}

int Scene2D::GetInitCode() const
{
	return this->initCode;
}

const char *Scene2D::GetInitStageName() const
{
	switch (this->initStage)
	{
	case SCENE_INIT_VIDEO_OPEN: return "sceVideoOutOpen";
	case SCENE_INIT_FLIP_QUEUE: return "sceKernelCreateEqueue";
	case SCENE_INIT_FLIP_EVENT: return "sceVideoOutAddFlipEvent";
	case SCENE_INIT_DIRECT_MEMORY: return "sceKernelAllocateDirectMemory";
	case SCENE_INIT_DIRECT_MAP: return "sceKernelMapDirectMemory";
	case SCENE_INIT_FRAME_BUFFERS: return "Puffer-Array";
	case SCENE_INIT_REGISTER_BUFFERS: return "sceVideoOutRegisterBuffers";
	case SCENE_INIT_FLIP_RATE: return "sceVideoOutSetFlipRate";
	default: return "unbekannt";
	}
}

void Scene2D::SetActiveFrameBuffer(int index)
{
	this->activeFrameBufferIdx = index;
}

void Scene2D::SubmitFlip(int frameID)
{
	sceVideoOutSubmitFlip(this->video, this->activeFrameBufferIdx, ORBIS_VIDEO_OUT_FLIP_VSYNC, frameID);
}

void Scene2D::FrameWait(int frameID)
{
	OrbisKernelEvent evt;
	int count;
	
	// If the video handle is not initialized, bail out. This is mostly a failsafe, this should never happen.
	// cleanupInit() setzt den Handle auf -1, daher reicht == 0 hier nicht.
	if(this->video < 0)
		return;
		
	for(;;)
	{
		OrbisVideoOutFlipStatus flipStatus;
		
		// Get the flip status and check the arg for the given frame ID
		sceVideoOutGetFlipStatus(video, &flipStatus);
		
		if(flipStatus.flipArg == frameID)
			break;
			
		// Wait on next flip event
		if(sceKernelWaitEqueue(this->flipQueue, &evt, 1, &count, 0) != 0)
			break;
	}
}

void Scene2D::FrameBufferSwap()
{
	// Swap the frame buffer for some perf
	this->activeFrameBufferIdx = (this->activeFrameBufferIdx + 1) % 2;
}

void Scene2D::FrameBufferClear()
{
	// Clear the screen with a white frame buffer
	Color blank = { 255, 255, 255 };
	FrameBufferFill(blank);
}

#ifdef GRAPHICS_USES_FONT
bool Scene2D::InitFont(FT_Face *face, const char *fontPath, int fontSize)
{
	int rc;
	
	rc = FT_New_Face(this->ftLib, fontPath, 0, face);
	
	if(rc < 0)
		return false;

	rc = FT_Set_Pixel_Sizes(*face, 0, fontSize);
	
	if(rc < 0)
		return false;
	
	return true;
}
#endif

void Scene2D::FrameBufferFill(Color color)
{
	DrawRectangle(0, 0, this->width, this->height, color);
}

void Scene2D::DrawPixel(int x, int y, Color color)
{
	// Get pixel location based on pitch
	int pixel = (y * this->width) + x;
	
	// Encode to 24-bit color
	uint32_t encodedColor = 0x80000000 + (color.r << 16) + (color.g << 8) + color.b;
	
	// Draw to the frame buffer
	((uint32_t *)this->frameBuffers[this->activeFrameBufferIdx])[pixel] = encodedColor;
}

void Scene2D::DrawRectangle(int x, int y, int w, int h, Color color)
{
	int xPos, yPos;
	
	// Draw row-by-row, column-by-column
	for(yPos = y; yPos < y + h; yPos++)
	{
		for(xPos = x; xPos < x + w; xPos++)
		{
			DrawPixel(xPos, yPos, color);
		}
	}
}

#ifdef GRAPHICS_USES_FONT
void Scene2D::DrawText(char *txt, FT_Face face, int startX, int startY, Color bgColor, Color fgColor)
{
	int rc;
	int xOffset = 0;
	int yOffset = 0;
	
	// Get the glyph slot for bitmap and font metrics
	FT_GlyphSlot slot = face->glyph;
	
	// Iterate each character of the text to write to the screen
	for(int n = 0; n < strlen(txt); n++)
	{
		FT_UInt glyph_index;

        // Get the glyph for the ASCII code
        glyph_index = FT_Get_Char_Index(face, txt[n]);

        // Load and render in 8-bit color
        rc = FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);

        if (rc)
            continue;

        rc = FT_Render_Glyph(slot, ft_render_mode_normal);

        if (rc)
            continue;

        // If we get a newline, increment the y offset, reset the x offset, and skip to the next character
        if (txt[n] == '\n')
        {
            xOffset = 0;
            yOffset += slot->bitmap.width * 2;

            continue;
        }

        // Parse and write the bitmap to the frame buffer
        for (int yPos = 0; yPos < slot->bitmap.rows; yPos++)
        {
            for (int xPos = 0; xPos < slot->bitmap.width; xPos++)
            {
                // Decode the 8-bit bitmap
                char pixel = slot->bitmap.buffer[(yPos * slot->bitmap.width) + xPos];

                // Get new pixel coordinates to account for the character position and baseline, as well as newlines
                int x = startX + xPos + xOffset + slot->bitmap_left;
                int y = startY + yPos + yOffset - slot->bitmap_top;

                // Linearly interpolate between the foreground and background for smoother rendering
				uint8_t r = (pixel * fgColor.r) / 255;
				uint8_t g = (pixel * fgColor.g) / 255;
				uint8_t b = (pixel * fgColor.b) / 255;

                // Create new color struct with lerp'd values
                Color finalColor = { r, g, b };

                // We need to do bounds checking before commiting the pixel write due to our transformations, or we
                // could write out-of-bounds of the frame buffer
                if (x < 0 || y < 0 || x >= this->width || y >= this->height)
                    continue;

                // If the pixel in the bitmap isn't blank, we'll draw it
                if(pixel != 0x00)
                    this->DrawPixel(x, y, finalColor);
            }
        }

        // Increment x offset for the next character
        xOffset += slot->advance.x >> 6;
    }
}
#endif

