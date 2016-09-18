// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2016 Magnus Norddahl
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_renderbuffers.cpp
** Render buffers used during rendering
**
*/

#include "gl/system/gl_system.h"
#include "files.h"
#include "m_swap.h"
#include "v_video.h"
#include "gl/gl_functions.h"
#include "vectors.h"
#include "gl/system/gl_interface.h"
#include "gl/system/gl_framebuffer.h"
#include "gl/system/gl_cvars.h"
#include "gl/system/gl_debug.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_renderbuffers.h"
#include "w_wad.h"
#include "i_system.h"
#include "doomerrors.h"
#include <random>

CVAR(Int, gl_multisample, 1, CVAR_ARCHIVE|CVAR_GLOBALCONFIG);
CVAR(Bool, gl_renderbuffers, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)

//==========================================================================
//
// Initialize render buffers and textures used in rendering passes
//
//==========================================================================

FGLRenderBuffers::FGLRenderBuffers()
{
	for (int i = 0; i < NumPipelineTextures; i++)
	{
		mPipelineTexture[i] = 0;
		mPipelineFB[i] = 0;
	}

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&mOutputFB);
	glGetIntegerv(GL_MAX_SAMPLES, &mMaxSamples);
}

//==========================================================================
//
// Free render buffer resources
//
//==========================================================================

FGLRenderBuffers::~FGLRenderBuffers()
{
	ClearScene();
	ClearPipeline();
	ClearEyeBuffers();
	ClearBloom();
	ClearExposureLevels();
	ClearAmbientOcclusion();
}

void FGLRenderBuffers::ClearScene()
{
	DeleteFrameBuffer(mSceneFB);
	DeleteTexture(mSceneMultisample);
	DeleteTexture(mSceneDepthStencil);
}

void FGLRenderBuffers::ClearPipeline()
{
	for (int i = 0; i < NumPipelineTextures; i++)
	{
		DeleteFrameBuffer(mPipelineFB[i]);
		DeleteTexture(mPipelineTexture[i]);
	}
}

void FGLRenderBuffers::ClearBloom()
{
	for (int i = 0; i < NumBloomLevels; i++)
	{
		auto &level = BloomLevels[i];
		DeleteFrameBuffer(level.HFramebuffer);
		DeleteFrameBuffer(level.VFramebuffer);
		DeleteTexture(level.HTexture);
		DeleteTexture(level.VTexture);
		level = FGLBloomTextureLevel();
	}
}

void FGLRenderBuffers::ClearExposureLevels()
{
	for (auto &level : ExposureLevels)
	{
		DeleteTexture(level.Texture);
		DeleteFrameBuffer(level.Framebuffer);
	}
	ExposureLevels.Clear();
	DeleteTexture(ExposureTexture);
	DeleteFrameBuffer(ExposureFB);
}

void FGLRenderBuffers::ClearEyeBuffers()
{
	for (auto handle : mEyeFBs)
		DeleteFrameBuffer(handle);

	for (auto handle : mEyeTextures)
		DeleteTexture(handle);

	mEyeTextures.Clear();
	mEyeFBs.Clear();
}

void FGLRenderBuffers::ClearAmbientOcclusion()
{
	DeleteFrameBuffer(AmbientFB0);
	DeleteFrameBuffer(AmbientFB1);
	DeleteTexture(AmbientTexture0);
	DeleteTexture(AmbientTexture1);
	DeleteTexture(AmbientRandomTexture);
}

void FGLRenderBuffers::DeleteTexture(GLuint &handle)
{
	if (handle != 0)
		glDeleteTextures(1, &handle);
	handle = 0;
}

void FGLRenderBuffers::DeleteRenderBuffer(GLuint &handle)
{
	if (handle != 0)
		glDeleteRenderbuffers(1, &handle);
	handle = 0;
}

void FGLRenderBuffers::DeleteFrameBuffer(GLuint &handle)
{
	if (handle != 0)
		glDeleteFramebuffers(1, &handle);
	handle = 0;
}

//==========================================================================
//
// Makes sure all render buffers have sizes suitable for rending at the
// specified resolution
//
//==========================================================================

bool FGLRenderBuffers::Setup(int width, int height, int sceneWidth, int sceneHeight)
{
	if (gl_renderbuffers != BuffersActive)
	{
		if (BuffersActive)
			glBindFramebuffer(GL_FRAMEBUFFER, mOutputFB);
		BuffersActive = gl_renderbuffers;
		GLRenderer->mShaderManager->ResetFixedColormap();
	}

	if (!IsEnabled())
		return false;
		
	if (width <= 0 || height <= 0)
		I_FatalError("Requested invalid render buffer sizes: screen = %dx%d", width, height);

	int samples = clamp((int)gl_multisample, 0, mMaxSamples);

	GLint activeTex;
	GLint textureBinding;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);

	if (width == mWidth && height == mHeight && mSamples != samples)
	{
		CreateScene(mWidth, mHeight, samples);
		mSamples = samples;
	}
	else if (width != mWidth || height != mHeight)
	{
		CreatePipeline(width, height);
		CreateScene(width, height, samples);
		mWidth = width;
		mHeight = height;
		mSamples = samples;
	}

	// Bloom bluring buffers need to match the scene to avoid bloom bleeding artifacts
	if (mSceneWidth != sceneWidth || mSceneHeight != sceneHeight)
	{
		CreateBloom(sceneWidth, sceneHeight);
		CreateExposureLevels(sceneWidth, sceneHeight);
		CreateAmbientOcclusion(sceneWidth, sceneHeight);
		mSceneWidth = sceneWidth;
		mSceneHeight = sceneHeight;
	}

	glBindTexture(GL_TEXTURE_2D, textureBinding);
	glActiveTexture(activeTex);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (FailedCreate)
	{
		ClearScene();
		ClearPipeline();
		ClearEyeBuffers();
		ClearBloom();
		ClearExposureLevels();
		mWidth = 0;
		mHeight = 0;
		mSamples = 0;
		mSceneWidth = 0;
		mSceneHeight = 0;
	}

	return !FailedCreate;
}

//==========================================================================
//
// Creates the scene buffers
//
//==========================================================================

void FGLRenderBuffers::CreateScene(int width, int height, int samples)
{
	ClearScene();

	if (samples > 1)
	{
		mSceneMultisample = Create2DMultisampleTexture("SceneMultisample", GL_RGBA16F, width, height, samples, false);
		mSceneDepthStencil = Create2DMultisampleTexture("SceneDepthStencil", GL_DEPTH24_STENCIL8, width, height, samples, false);
	}
	else
	{
		mSceneDepthStencil = Create2DTexture("SceneDepthStencil", GL_DEPTH24_STENCIL8, width, height);
	}

	mSceneFB = CreateFrameBuffer("SceneFB", samples > 1 ? mSceneMultisample : mPipelineTexture[0], mSceneDepthStencil, samples > 1);
}

//==========================================================================
//
// Creates the buffers needed for post processing steps
//
//==========================================================================

void FGLRenderBuffers::CreatePipeline(int width, int height)
{
	ClearPipeline();
	ClearEyeBuffers();

	for (int i = 0; i < NumPipelineTextures; i++)
	{
		mPipelineTexture[i] = Create2DTexture("PipelineTexture", GL_RGBA16F, width, height);
		mPipelineFB[i] = CreateFrameBuffer("PipelineFB", mPipelineTexture[i]);
	}
}

//==========================================================================
//
// Creates bloom pass working buffers
//
//==========================================================================

void FGLRenderBuffers::CreateBloom(int width, int height)
{
	ClearBloom();
	
	// No scene, no bloom!
	if (width <= 0 || height <= 0)
		return;

	int bloomWidth = MAX(width / 2, 1);
	int bloomHeight = MAX(height / 2, 1);
	for (int i = 0; i < NumBloomLevels; i++)
	{
		auto &level = BloomLevels[i];
		level.Width = MAX(bloomWidth / 2, 1);
		level.Height = MAX(bloomHeight / 2, 1);

		level.VTexture = Create2DTexture("Bloom.VTexture", GL_RGBA16F, level.Width, level.Height);
		level.HTexture = Create2DTexture("Bloom.HTexture", GL_RGBA16F, level.Width, level.Height);
		level.VFramebuffer = CreateFrameBuffer("Bloom.VFramebuffer", level.VTexture);
		level.HFramebuffer = CreateFrameBuffer("Bloom.HFramebuffer", level.HTexture);

		bloomWidth = level.Width;
		bloomHeight = level.Height;
	}
}

//==========================================================================
//
// Creates ambient occlusion working buffers
//
//==========================================================================

void FGLRenderBuffers::CreateAmbientOcclusion(int width, int height)
{
	ClearAmbientOcclusion();

	if (width <= 0 || height <= 0)
		return;

	AmbientWidth = width / 2;
	AmbientHeight = height / 2;
	AmbientTexture0 = Create2DTexture("AmbientTexture0", GL_RG32F, AmbientWidth, AmbientHeight);
	AmbientTexture1 = Create2DTexture("AmbientTexture1", GL_RG32F, AmbientWidth, AmbientHeight);
	AmbientFB0 = CreateFrameBuffer("AmbientFB0", AmbientTexture0);
	AmbientFB1 = CreateFrameBuffer("AmbientFB1", AmbientTexture1);

	int16_t randomValues[16 * 4];
	std::mt19937 generator(1337);
	std::uniform_real_distribution<double> distribution(-1.0, 1.0);
	for (int i = 0; i < 16; i++)
	{
		double num_directions = 8.0; // Must be same as the define in ssao.fp
		double angle = 2.0 * M_PI * distribution(generator) / num_directions;
		double x = cos(angle);
		double y = sin(angle);
		double z = distribution(generator);
		double w = distribution(generator);

		randomValues[i * 4 + 0] = (int16_t)clamp(x * 32767.0, -32768.0, 32767.0);
		randomValues[i * 4 + 1] = (int16_t)clamp(y * 32767.0, -32768.0, 32767.0);
		randomValues[i * 4 + 2] = (int16_t)clamp(z * 32767.0, -32768.0, 32767.0);
		randomValues[i * 4 + 3] = (int16_t)clamp(w * 32767.0, -32768.0, 32767.0);
	}

	AmbientRandomTexture = Create2DTexture("AmbientRandomTexture", GL_RGBA16_SNORM, 4, 4, randomValues);
}

//==========================================================================
//
// Creates camera exposure level buffers
//
//==========================================================================

void FGLRenderBuffers::CreateExposureLevels(int width, int height)
{
	ClearExposureLevels();

	int i = 0;
	do
	{
		width = MAX(width / 2, 1);
		height = MAX(height / 2, 1);

		FString textureName, fbName;
		textureName.Format("Exposure.Texture%d", i);
		fbName.Format("Exposure.Framebuffer%d", i);
		i++;

		FGLExposureTextureLevel level;
		level.Width = width;
		level.Height = height;
		level.Texture = Create2DTexture(textureName, GL_R32F, level.Width, level.Height);
		level.Framebuffer = CreateFrameBuffer(fbName, level.Texture);
		ExposureLevels.Push(level);
	} while (width > 1 || height > 1);

	ExposureTexture = Create2DTexture("Exposure.CameraTexture", GL_R32F, 1, 1);
	ExposureFB = CreateFrameBuffer("Exposure.CameraFB", ExposureTexture);

	FirstExposureFrame = true;
}

//==========================================================================
//
// Creates eye buffers if needed
//
//==========================================================================

void FGLRenderBuffers::CreateEyeBuffers(int eye)
{
	if (mEyeFBs.Size() > eye)
		return;

	GLint activeTex, textureBinding, frameBufferBinding;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &frameBufferBinding);

	while (mEyeFBs.Size() <= eye)
	{
		GLuint texture = Create2DTexture("EyeTexture", GL_RGBA16F, mWidth, mHeight);
		mEyeTextures.Push(texture);
		mEyeFBs.Push(CreateFrameBuffer("EyeFB", texture));
	}

	glBindTexture(GL_TEXTURE_2D, textureBinding);
	glActiveTexture(activeTex);
	glBindFramebuffer(GL_FRAMEBUFFER, frameBufferBinding);
}

//==========================================================================
//
// Creates a 2D texture defaulting to linear filtering and clamp to edge
//
//==========================================================================

GLuint FGLRenderBuffers::Create2DTexture(const FString &name, GLuint format, int width, int height, const void *data)
{
	GLuint handle = 0;
	glGenTextures(1, &handle);
	glBindTexture(GL_TEXTURE_2D, handle);
	FGLDebug::LabelObject(GL_TEXTURE, handle, name);

	GLenum dataformat, datatype;
	switch (format)
	{
	case GL_RGBA8:				dataformat = GL_RGBA; datatype = GL_UNSIGNED_BYTE; break;
	case GL_RGBA16:				dataformat = GL_RGBA; datatype = GL_UNSIGNED_SHORT; break;
	case GL_RGBA16F:			dataformat = GL_RGBA; datatype = GL_FLOAT; break;
	case GL_RGBA32F:			dataformat = GL_RGBA; datatype = GL_FLOAT; break;
	case GL_R32F:				dataformat = GL_RED; datatype = GL_FLOAT; break;
	case GL_RG32F:				dataformat = GL_RG; datatype = GL_FLOAT; break;
	case GL_DEPTH_COMPONENT24:	dataformat = GL_DEPTH_COMPONENT; datatype = GL_FLOAT; break;
	case GL_STENCIL_INDEX8:		dataformat = GL_STENCIL_INDEX; datatype = GL_INT; break;
	case GL_DEPTH24_STENCIL8:	dataformat = GL_DEPTH_STENCIL; datatype = GL_UNSIGNED_INT_24_8; break;
	case GL_RGBA16_SNORM:		dataformat = GL_RGBA; datatype = GL_SHORT; break;
	default: I_FatalError("Unknown format passed to FGLRenderBuffers.Create2DTexture");
	}

	glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, dataformat, datatype, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return handle;
}

GLuint FGLRenderBuffers::Create2DMultisampleTexture(const FString &name, GLuint format, int width, int height, int samples, bool fixedSampleLocations)
{
	GLuint handle = 0;
	glGenTextures(1, &handle);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, handle);
	FGLDebug::LabelObject(GL_TEXTURE, handle, name);
	glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, format, width, height, fixedSampleLocations);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
	return handle;
}

//==========================================================================
//
// Creates a render buffer
//
//==========================================================================

GLuint FGLRenderBuffers::CreateRenderBuffer(const FString &name, GLuint format, int width, int height)
{
	GLuint handle = 0;
	glGenRenderbuffers(1, &handle);
	glBindRenderbuffer(GL_RENDERBUFFER, handle);
	FGLDebug::LabelObject(GL_RENDERBUFFER, handle, name);
	glRenderbufferStorage(GL_RENDERBUFFER, format, width, height);
	return handle;
}

GLuint FGLRenderBuffers::CreateRenderBuffer(const FString &name, GLuint format, int samples, int width, int height)
{
	if (samples <= 1)
		return CreateRenderBuffer(name, format, width, height);

	GLuint handle = 0;
	glGenRenderbuffers(1, &handle);
	glBindRenderbuffer(GL_RENDERBUFFER, handle);
	FGLDebug::LabelObject(GL_RENDERBUFFER, handle, name);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, format, width, height);
	return handle;
}

//==========================================================================
//
// Creates a frame buffer
//
//==========================================================================

GLuint FGLRenderBuffers::CreateFrameBuffer(const FString &name, GLuint colorbuffer)
{
	GLuint handle = 0;
	glGenFramebuffers(1, &handle);
	glBindFramebuffer(GL_FRAMEBUFFER, handle);
	FGLDebug::LabelObject(GL_FRAMEBUFFER, handle, name);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorbuffer, 0);
	if (CheckFrameBufferCompleteness())
		ClearFrameBuffer(false, false);
	return handle;
}

GLuint FGLRenderBuffers::CreateFrameBuffer(const FString &name, GLuint colorbuffer, GLuint depthstencil, bool multisample)
{
	GLuint handle = 0;
	glGenFramebuffers(1, &handle);
	glBindFramebuffer(GL_FRAMEBUFFER, handle);
	FGLDebug::LabelObject(GL_FRAMEBUFFER, handle, name);
	if (multisample)
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, colorbuffer, 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, depthstencil, 0);
	}
	else
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorbuffer, 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthstencil, 0);
	}
	if (CheckFrameBufferCompleteness())
		ClearFrameBuffer(true, true);
	return handle;
}

//==========================================================================
//
// Verifies that the frame buffer setup is valid
//
//==========================================================================

bool FGLRenderBuffers::CheckFrameBufferCompleteness()
{
	GLenum result = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (result == GL_FRAMEBUFFER_COMPLETE)
		return true;

	FailedCreate = true;

	if (gl_debug_level > 0)
	{
		FString error = "glCheckFramebufferStatus failed: ";
		switch (result)
		{
		default: error.AppendFormat("error code %d", (int)result); break;
		case GL_FRAMEBUFFER_UNDEFINED: error << "GL_FRAMEBUFFER_UNDEFINED"; break;
		case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: error << "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT"; break;
		case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: error << "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT"; break;
		case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER: error << "GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER"; break;
		case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER: error << "GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER"; break;
		case GL_FRAMEBUFFER_UNSUPPORTED: error << "GL_FRAMEBUFFER_UNSUPPORTED"; break;
		case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE: error << "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE"; break;
		case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS: error << "GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS"; break;
		}
		Printf("%s\n", error.GetChars());
	}

	return false;
}

//==========================================================================
//
// Clear frame buffer to make sure it never contains uninitialized data
//
//==========================================================================

void FGLRenderBuffers::ClearFrameBuffer(bool stencil, bool depth)
{
	GLboolean scissorEnabled;
	GLint stencilValue;
	GLdouble depthValue;
	glGetBooleanv(GL_SCISSOR_TEST, &scissorEnabled);
	glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &stencilValue);
	glGetDoublev(GL_DEPTH_CLEAR_VALUE, &depthValue);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0, 0.0, 0.0, 0.0);
	glClearDepth(0.0);
	glClearStencil(0);
	GLenum flags = GL_COLOR_BUFFER_BIT;
	if (stencil)
		flags |= GL_STENCIL_BUFFER_BIT;
	if (depth)
		flags |= GL_DEPTH_BUFFER_BIT;
	glClear(flags);
	glClearStencil(stencilValue);
	glClearDepth(depthValue);
	if (scissorEnabled)
		glEnable(GL_SCISSOR_TEST);
}

//==========================================================================
//
// Resolves the multisample frame buffer by copying it to the scene texture
//
//==========================================================================

void FGLRenderBuffers::BlitSceneToTexture()
{
	mCurrentPipelineTexture = 0;

	if (mSamples <= 1)
		return;

	glBindFramebuffer(GL_READ_FRAMEBUFFER, mSceneFB);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mPipelineFB[mCurrentPipelineTexture]);
	glBlitFramebuffer(0, 0, mWidth, mHeight, 0, 0, mWidth, mHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);

	if ((gl.flags & RFL_INVALIDATE_BUFFER) != 0)
	{
		GLenum attachments[2] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_STENCIL_ATTACHMENT };
		glInvalidateFramebuffer(GL_READ_FRAMEBUFFER, 2, attachments);
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

//==========================================================================
//
// Eye textures and their frame buffers
//
//==========================================================================

void FGLRenderBuffers::BlitToEyeTexture(int eye)
{
	CreateEyeBuffers(eye);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, mPipelineFB[mCurrentPipelineTexture]);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mEyeFBs[eye]);
	glBlitFramebuffer(0, 0, mWidth, mHeight, 0, 0, mWidth, mHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);

	if ((gl.flags & RFL_INVALIDATE_BUFFER) != 0)
	{
		GLenum attachments[2] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_STENCIL_ATTACHMENT };
		glInvalidateFramebuffer(GL_READ_FRAMEBUFFER, 2, attachments);
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

void FGLRenderBuffers::BindEyeTexture(int eye, int texunit)
{
	CreateEyeBuffers(eye);
	glActiveTexture(GL_TEXTURE0 + texunit);
	glBindTexture(GL_TEXTURE_2D, mEyeTextures[eye]);
}

void FGLRenderBuffers::BindEyeFB(int eye, bool readBuffer)
{
	CreateEyeBuffers(eye);
	glBindFramebuffer(readBuffer ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER, mEyeFBs[eye]);
}

//==========================================================================
//
// Makes the scene frame buffer active (multisample, depth, stecil, etc.)
//
//==========================================================================

void FGLRenderBuffers::BindSceneFB()
{
	glBindFramebuffer(GL_FRAMEBUFFER, mSceneFB);
}

//==========================================================================
//
// Binds the scene color texture to the specified texture unit
//
//==========================================================================

void FGLRenderBuffers::BindSceneColorTexture(int index)
{
	glActiveTexture(GL_TEXTURE0 + index);
	if (mSamples > 1)
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, mSceneMultisample);
	else
		glBindTexture(GL_TEXTURE_2D, mPipelineTexture[0]);
}

//==========================================================================
//
// Binds the depth texture to the specified texture unit
//
//==========================================================================

void FGLRenderBuffers::BindSceneDepthTexture(int index)
{
	glActiveTexture(GL_TEXTURE0 + index);
	if (mSamples > 1)
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, mSceneDepthStencil);
	else
		glBindTexture(GL_TEXTURE_2D, mSceneDepthStencil);
}

//==========================================================================
//
// Binds the current scene/effect/hud texture to the specified texture unit
//
//==========================================================================

void FGLRenderBuffers::BindCurrentTexture(int index)
{
	glActiveTexture(GL_TEXTURE0 + index);
	glBindTexture(GL_TEXTURE_2D, mPipelineTexture[mCurrentPipelineTexture]);
}

//==========================================================================
//
// Makes the frame buffer for the current texture active 
//
//==========================================================================

void FGLRenderBuffers::BindCurrentFB()
{
	glBindFramebuffer(GL_FRAMEBUFFER, mPipelineFB[mCurrentPipelineTexture]);
}

//==========================================================================
//
// Makes the frame buffer for the next texture active
//
//==========================================================================

void FGLRenderBuffers::BindNextFB()
{
	int out = (mCurrentPipelineTexture + 1) % NumPipelineTextures;
	glBindFramebuffer(GL_FRAMEBUFFER, mPipelineFB[out]);
}

//==========================================================================
//
// Next pipeline texture now contains the output
//
//==========================================================================

void FGLRenderBuffers::NextTexture()
{
	mCurrentPipelineTexture = (mCurrentPipelineTexture + 1) % NumPipelineTextures;
}

//==========================================================================
//
// Makes the screen frame buffer active
//
//==========================================================================

void FGLRenderBuffers::BindOutputFB()
{
	glBindFramebuffer(GL_FRAMEBUFFER, mOutputFB);
}

//==========================================================================
//
// Returns true if render buffers are supported and should be used
//
//==========================================================================

bool FGLRenderBuffers::IsEnabled()
{
	return BuffersActive && !gl.legacyMode && !FailedCreate;
}

bool FGLRenderBuffers::FailedCreate = false;
bool FGLRenderBuffers::BuffersActive = false;
