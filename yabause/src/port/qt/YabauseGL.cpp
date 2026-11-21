/*  Copyright 2005 Guillaume Duhamel
	Copyright 2005-2006 Theo Berkau
	Copyright 2008 Filipe Azevedo <pasnox@gmail.com>

	This file is part of Yabause.

	Yabause is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Yabause is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Yabause; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
#include "YabauseGL.h"
#include "QtYabause.h"
#include "Settings.h"
#include <QKeyEvent>
#include <QApplication>
#include <QOpenGLContext>
#include <atomic>
#include <cstring>

extern "C" {
#include "movie.h"
}

/* ---- Swap interval -------------------------------------------------------
   Qt only reads QSurfaceFormat::swapInterval() when the context is created,
   and knows nothing about "adaptive" (negative) intervals. The interval has to
   change while the emulator runs (a fast forward or a slowed down game must
   not be held to the refresh rate of the display), so it is set through the
   swap control extension of the platform, looked up at run time through
   QOpenGLContext::getProcAddress(): no platform header or library is needed
   to build. When none of them is there (macOS, a Wayland compositor without
   EGL access...) the value of the surface format given at start-up stays in
   force and the mode only changes at the next launch. */
namespace {

std::atomic<int>  gSwapMode(0);
std::atomic<bool> gLowLatency(false);

#ifdef Q_OS_WIN
#define YGL_SWAPAPI __stdcall
#else
#define YGL_SWAPAPI
#endif

typedef int           (YGL_SWAPAPI *PFN_wglSwapIntervalEXT)(int);
typedef void *        (*PFN_glXGetCurrentDisplay)(void);
typedef unsigned long (*PFN_glXGetCurrentDrawable)(void);
typedef void          (*PFN_glXSwapIntervalEXT)(void *, unsigned long, int);
typedef int           (*PFN_glXSwapIntervalMESA)(unsigned int);
typedef const char *  (*PFN_glXQueryExtensionsString)(void *, int);
typedef void *        (*PFN_eglGetCurrentDisplay)(void);
typedef unsigned int  (*PFN_eglSwapInterval)(void *, int);

/* true when the driver took the interval. interval: -1 adaptive, 0 off, 1 on. */
bool nativeSetSwapInterval(QOpenGLContext *ctx, int interval)
{
  if (ctx == nullptr)
    return false;

#if defined(Q_OS_WIN)
  PFN_wglSwapIntervalEXT fn =
    reinterpret_cast<PFN_wglSwapIntervalEXT>(ctx->getProcAddress("wglSwapIntervalEXT"));
  /* returns FALSE, without side effect, for -1 when the tear extension is
     missing */
  return fn != nullptr && fn(interval) != 0;

#elif defined(Q_OS_MACOS) || defined(Q_OS_DARWIN)
  return false;

#else
  /* GLX (X11) */
  PFN_glXGetCurrentDisplay  xDisplay =
    reinterpret_cast<PFN_glXGetCurrentDisplay>(ctx->getProcAddress("glXGetCurrentDisplay"));
  PFN_glXGetCurrentDrawable xDrawable =
    reinterpret_cast<PFN_glXGetCurrentDrawable>(ctx->getProcAddress("glXGetCurrentDrawable"));
  PFN_glXSwapIntervalEXT    xSwapExt =
    reinterpret_cast<PFN_glXSwapIntervalEXT>(ctx->getProcAddress("glXSwapIntervalEXT"));

  if (xDisplay != nullptr && xDrawable != nullptr) {
    void *dpy = xDisplay();
    unsigned long drawable = xDrawable();
    if (dpy != nullptr && drawable != 0) {
      if (xSwapExt != nullptr) {
        if (interval < 0) {
          /* A negative interval without GLX_EXT_swap_control_tear is a GLX
             protocol error, which Xlib treats as fatal: check first. */
          PFN_glXQueryExtensionsString xQuery =
            reinterpret_cast<PFN_glXQueryExtensionsString>(ctx->getProcAddress("glXQueryExtensionsString"));
          const char *ext = (xQuery != nullptr) ? xQuery(dpy, 0) : nullptr;
          if (ext == nullptr || std::strstr(ext, "GLX_EXT_swap_control_tear") == nullptr)
            return false;
        }
        xSwapExt(dpy, drawable, interval);
        return true;
      }
      PFN_glXSwapIntervalMESA xMesa =
        reinterpret_cast<PFN_glXSwapIntervalMESA>(ctx->getProcAddress("glXSwapIntervalMESA"));
      if (xMesa != nullptr && interval >= 0)
        return xMesa(static_cast<unsigned int>(interval)) == 0;
    }
  }

  /* EGL (Wayland, EGL builds of Qt on X11). EGL has no adaptive interval. */
  PFN_eglGetCurrentDisplay eDisplay =
    reinterpret_cast<PFN_eglGetCurrentDisplay>(ctx->getProcAddress("eglGetCurrentDisplay"));
  PFN_eglSwapInterval      eSwap =
    reinterpret_cast<PFN_eglSwapInterval>(ctx->getProcAddress("eglSwapInterval"));
  if (eDisplay != nullptr && eSwap != nullptr && interval >= 0) {
    void *dpy = eDisplay();
    if (dpy != nullptr)
      return eSwap(dpy, interval) != 0;
  }
  return false;
#endif
}

} // namespace

void YabauseGL::setRequestedSwapMode(int mode, bool lowLatency)
{
  if (mode < SwapOff || mode > SwapAdaptive)
    mode = SwapOff;
  gSwapMode.store(mode);
  gLowLatency.store(lowLatency);
}


YabauseGL::YabauseGL( ) : QOpenGLWindow()
{
  QSurfaceFormat format;
  format.setDepthBufferSize(24);
  format.setStencilBufferSize(8);
  format.setRedBufferSize(8);
  format.setGreenBufferSize(8);
  format.setBlueBufferSize(8);
  format.setAlphaBufferSize(8);
  {
    /* Mode of the last session: gives the right interval from the very first
       swap, and is the only thing that counts on platforms where the
       interval cannot be changed afterwards (see nativeSetSwapInterval). */
    Settings *s = QtYabause::settings();
    const int mode = (s != nullptr) ? s->value("Video/SwapMode", static_cast<int>(SwapOff)).toInt() : static_cast<int>(SwapOff);
    const bool lowLatency = (s != nullptr) && s->value("Video/LowLatencyPresent", false).toBool();
    setRequestedSwapMode(mode, lowLatency);
    format.setSwapInterval(mode == SwapOff ? 0 : 1);
  }
  format.setRenderableType(QSurfaceFormat::OpenGL);

#ifdef _OGL3_
  format.setMajorVersion(4);
  format.setMinorVersion(3);
#endif

#ifdef _OGLES3_
  format.setMajorVersion(3);
  format.setMinorVersion(0);
  format.setRenderableType(QSurfaceFormat::OpenGLES);
#endif

#ifdef _OGLES31_
  format.setMajorVersion(3);
  format.setMinorVersion(1);
  format.setRenderableType(QSurfaceFormat::OpenGLES);
#endif

  format.setProfile(QSurfaceFormat::CoreProfile);
  setFormat(format);
  mPause = true;
}

void YabauseGL::requestFrame() {
  QApplication::postEvent(this, new FrameRequest());
}
void YabauseGL::pauseFrame() {
  QApplication::postEvent(this, new FrameStop());
}

void YabauseGL::pause(bool pause) {
  if (pause != mPause) {
    if (!pause) {
      mPause = false;
      updateView();
      requestFrame();
    } else {
      pauseFrame();
    }
  }
}

void YabauseGL::initializeGL()
{
  initializeOpenGLFunctions();
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

bool YabauseGL::event(QEvent *event)
{
    switch (event->type()) {
    case FrameRequest::mType:
          /* Emulation is driven by these posted events on the GUI thread:
             each frame runs YabauseExec() and posts the next FrameRequest.
             Any modal dialog opened from inside emulation -- the SH2
             breakpoint dialog above all -- spins a nested event loop, that
             loop finds the queued FrameRequest, and YabauseExec() is
             entered again from inside itself.

             The visible symptom is a debugger that opens on registers
             belonging to a later moment than the breakpoint, which quietly
             invalidates every measurement taken through it. The real
             hazard is worse: re-entering the interpreter while an outer
             frame is suspended mid-instruction corrupts emulator state in
             ways that have nothing to do with debugging.

             Drop nested requests. The outer invocation posts a fresh one
             as soon as it returns, so the frame chain is never broken. */
          if (!mPause && !mInFrame) {
            mInFrame = true;
            YabauseExec();
            mInFrame = false;
            /* A pause asked for this exact frame (movie.c: seek target,
               end of a read-only playback) cannot go through pause(): the
               FrameStop it posts is queued behind the FrameRequest posted
               here, so one more frame would run first. Stop the chain
               instead; UIYabause then pauses the emulation thread. */
            if (MovieConsumePauseRequest()) {
              mPause = true;
              emit pauseRequested();
            } else {
              requestFrame();
            }
          }
          return true;
    case FrameStop::mType:
          mPause = true;
          emit emulationPaused();
          return true;
    default:
        return QWindow::event(event);
    }
}

bool YabauseGL::stepFrame()
{
  if (mInFrame)
    return false;
  mInFrame = true;
  YabauseExec();
  mInFrame = false;
  /* already paused: nothing more to do with a pause request */
  MovieConsumePauseRequest();
  return true;
}

/* Interval wanted right now. The mode of the settings only holds while the
   core really paces the frames on the console rate: with fast forward, a speed
   other than 100 %, or the limiter off (automatic frame skip) a blocking swap
   would hold the emulation to the refresh rate of the display. */
void YabauseGL::applySwapInterval()
{
  int want = 0;
  const bool paced = !YabauseGetTurbo() && (YabauseGetSpeedPercent() == 100) && !isAutoFrameSkip();
  if (paced) {
    switch (gSwapMode.load()) {
    case SwapVSync:    want = 1;  break;
    case SwapAdaptive: want = -1; break;
    default:           want = 0;  break;
    }
  }

  if (want == mAppliedInterval)
    return;
  /* Remembered even when the driver refuses, so that a platform without swap
     control is not asked again at every frame. */
  mAppliedInterval = want;

  QOpenGLContext *ctx = context();
  if (!nativeSetSwapInterval(ctx, want) && want < 0)
    nativeSetSwapInterval(ctx, 1); /* no tear extension: plain vertical sync */
}

void YabauseGL::swapBuffers()
{
  applySwapInterval();
  context()->swapBuffers(context()->surface());
  /* Low latency: do not let the driver run ahead. glFinish() returns once the
     frame has been taken by the display. */
  if (gLowLatency.load() && mAppliedInterval != 0)
    glFinish();
}

void YabauseGL::resizeGL( int w, int h )
{
  QOpenGLWindow::resizeGL(w, h);
  if ( VIDCore ) {
    VIDCore->Resize(0, 0, w, h, 0);
  }
    glInitialized();
 }

QImage YabauseGL::grabFrameBuffer()
{
	int width, height;

  pixel_t *vdp2texture = Vdp2DebugTexture(0xFF, &width, &height);
  QImage img((uchar *)vdp2texture, width, height, QImage::Format_RGB32);
	return img;
}

void YabauseGL::getScale(float *xRatio, float* yRatio, int* xUp, int *yUp) {
  if ( VIDCore ) {
    VIDCore->getScale(xRatio, yRatio, xUp, yUp);
  } else {
    *xRatio = 1.0;
    *yRatio = 1.0;
  }
}

void YabauseGL::updateView( const QSize& s )
{
	const QSize size = s.isValid() ? s : this->size();
	if ( VIDCore ) {
          VIDCore->Resize(0, 0, size.width(), size.height(), 0);
        }
}

/* Key events of this window already reach UIYabause::keyPressEvent()/
   keyReleaseEvent() through its event filter (UIYabause installs it on
   this window). Forwarding them here as well delivered every key twice
   to the peripherals, so they are only accepted here. */
void YabauseGL::keyPressEvent( QKeyEvent* e )
{
  e->accept();
}

void YabauseGL::keyReleaseEvent( QKeyEvent* e )
{
  e->accept();
}


extern "C"{
	int YuiRevokeOGLOnThisThread(){
		// Todo: needs to imp for async rendering
		return 0;
	}

	int YuiUseOGLOnThisThread(){
		// Todo: needs to imp for async rendering
		return 0;
	}
}
