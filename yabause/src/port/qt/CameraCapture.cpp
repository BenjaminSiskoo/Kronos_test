/*  Copyright 2026 Kronos contributors

    This file is part of Kronos.

    Kronos is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kronos is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kronos; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "CameraCapture.h"

#include <QDebug>

extern "C" {
#include "pclub.h"
}

#ifdef HAVE_OPENPNP_CAPTURE
#include "openpnp-capture.h"
#endif

/* A webcam gives at most 30 or 60 pictures a second: polling every 15 ms
   picks each of them up within one emulated frame. */
#define CAMERA_POLL_MS     15
/* Wait before trying again to open a camera that failed or went away. */
#define CAMERA_RETRY_POLLS (2000 / CAMERA_POLL_MS)

#ifdef HAVE_OPENPNP_CAPTURE
/* openpnp-capture prints a lot at its default level: errors only. */
static void cameraLog( uint32_t level, const char* string )
{
	if ( level <= 3 )
		qWarning( "openpnp-capture: %s", string );
}

static void cameraQuiet()
{
	static bool done = false;
	if ( !done )
	{
		Cap_installCustomLogFunction( cameraLog );
		Cap_setLogLevel( 3 );
		done = true;
	}
}
#endif

CameraCapture::CameraCapture( QObject* parent )
	: QObject( parent ),
	  mContext( 0 ),
	  mStream( -1 ),
	  mWidth( 0 ),
	  mHeight( 0 ),
	  mRetryTicks( 0 ),
	  mWarned( false )
{
	connect( &mTimer, SIGNAL( timeout() ), this, SLOT( poll() ) );
#ifdef HAVE_OPENPNP_CAPTURE
	mTimer.start( CAMERA_POLL_MS );
#endif
}

CameraCapture::~CameraCapture()
{
	mTimer.stop();
	closeStream();
#ifdef HAVE_OPENPNP_CAPTURE
	if ( mContext )
		Cap_releaseContext( (CapContext)mContext );
#endif
	mContext = 0;
}

bool CameraCapture::available()
{
#ifdef HAVE_OPENPNP_CAPTURE
	return true;
#else
	return false;
#endif
}

QList<CameraCapture::Device> CameraCapture::devices()
{
	QList<Device> list;
#ifdef HAVE_OPENPNP_CAPTURE
	cameraQuiet();
	CapContext ctx = Cap_createContext();
	if ( !ctx )
		return list;
	uint32_t count = Cap_getDeviceCount( ctx );
	for ( uint32_t i = 0; i < count; i++ )
	{
		Device d;
		const char* name = Cap_getDeviceName( ctx, i );
		const char* id = Cap_getDeviceUniqueID( ctx, i );
		d.name = QString::fromUtf8( name ? name : "" );
		d.id = QString::fromUtf8( id ? id : "" );
		if ( d.name.isEmpty() )
			d.name = QString( "Camera %1" ).arg( i + 1 );
		if ( d.id.isEmpty() )
			d.id = d.name;
		list << d;
	}
	Cap_releaseContext( ctx );
#endif
	return list;
}

void CameraCapture::setDevice( const QString& id )
{
	if ( id == mWanted )
		return;
	mWanted = id;
	closeStream();
	mRetryTicks = 0;
	mWarned = false;
}

void CameraCapture::closeStream()
{
#ifdef HAVE_OPENPNP_CAPTURE
	if ( mContext && mStream >= 0 )
	{
		Cap_closeStream( (CapContext)mContext, mStream );
		/* no frozen last picture: the board shows its test pattern again */
		PClubSetCameraFrame( NULL, 0, 0, 0 );
	}
#endif
	mStream = -1;
	mOpenedId.clear();
}

void CameraCapture::openStream()
{
#ifdef HAVE_OPENPNP_CAPTURE
	cameraQuiet();
	/* The device list is read when the context is made: make a new one at
	   each try so a camera plugged in after Kronos started is found. */
	if ( mContext )
		Cap_releaseContext( (CapContext)mContext );
	CapContext ctx = Cap_createContext();
	mContext = ctx;
	if ( !ctx )
		return;

	uint32_t count = Cap_getDeviceCount( ctx );
	int dev = -1;
	for ( uint32_t i = 0; i < count && dev < 0; i++ )
	{
		if ( mWanted.isEmpty() )
			dev = i;
		else
		{
			const char* uid = Cap_getDeviceUniqueID( ctx, i );
			const char* name = Cap_getDeviceName( ctx, i );
			if ( ( uid && mWanted == QString::fromUtf8( uid ) ) ||
			     ( name && mWanted == QString::fromUtf8( name ) ) )
				dev = i;
		}
	}
	if ( dev < 0 )
	{
		if ( !mWarned )
			qWarning( "Print Club camera: %s", mWanted.isEmpty() ? "no camera found" :
				qPrintable( QString( "camera \"%1\" not found" ).arg( mWanted ) ) );
		mWarned = true;
		return;
	}

	/* The board captures 640x240 (one field of 640x480): take the smallest
	   picture that is at least 640x480, else the biggest one; at equal size
	   the higher frame rate. */
	int32_t nformats = Cap_getNumFormats( ctx, dev );
	int best = -1;
	CapFormatInfo bestInfo;
	for ( int32_t f = 0; f < nformats; f++ )
	{
		CapFormatInfo info;
		if ( Cap_getFormatInfo( ctx, dev, f, &info ) != CAPRESULT_OK )
			continue;
		if ( info.width == 0 || info.height == 0 )
			continue;
		if ( best < 0 )
		{
			best = f; bestInfo = info;
			continue;
		}
		quint64 area = (quint64)info.width * info.height;
		quint64 bestArea = (quint64)bestInfo.width * bestInfo.height;
		bool big = info.width >= 640 && info.height >= 480;
		bool bestBig = bestInfo.width >= 640 && bestInfo.height >= 480;
		bool better;
		if ( big != bestBig )
			better = big;
		else if ( area != bestArea )
			better = big ? area < bestArea : area > bestArea;
		else
			better = info.fps > bestInfo.fps;
		if ( better )
		{
			best = f; bestInfo = info;
		}
	}
	if ( best < 0 )
	{
		if ( !mWarned )
			qWarning( "Print Club camera: no usable picture format" );
		mWarned = true;
		return;
	}

	CapStream s = Cap_openStream( ctx, dev, best );
	if ( s < 0 )
	{
		if ( !mWarned )
			qWarning( "Print Club camera: cannot open %s", Cap_getDeviceName( ctx, dev ) );
		mWarned = true;
		return;
	}
	mStream = s;
	mWidth = bestInfo.width;
	mHeight = bestInfo.height;
	mBuffer.resize( mWidth * mHeight * 3 );
	mOpenedId = mWanted;
	mWarned = false;
	qDebug( "Print Club camera: %s, %ux%u at %u fps", Cap_getDeviceName( ctx, dev ),
		bestInfo.width, bestInfo.height, bestInfo.fps );
#endif
}

void CameraCapture::poll()
{
#ifdef HAVE_OPENPNP_CAPTURE
	/* The camera is only on while a Print Club game runs. */
	if ( !PClubIsEnabled() || mWanted == "none" )
	{
		if ( mStream >= 0 )
			closeStream();
		mRetryTicks = 0;
		return;
	}

	if ( mStream < 0 )
	{
		if ( mRetryTicks > 0 )
		{
			mRetryTicks--;
			return;
		}
		openStream();
		if ( mStream < 0 )
		{
			mRetryTicks = CAMERA_RETRY_POLLS;
			return;
		}
	}

	CapContext ctx = (CapContext)mContext;
	if ( !Cap_isOpenStream( ctx, mStream ) )
	{
		/* Unplugged: try again later, the test pattern shows meanwhile. */
		qWarning( "Print Club camera: stream lost" );
		closeStream();
		mRetryTicks = CAMERA_RETRY_POLLS;
		return;
	}
	if ( !Cap_hasNewFrame( ctx, mStream ) )
		return;
	if ( Cap_captureFrame( ctx, mStream, mBuffer.data(), mBuffer.size() ) != CAPRESULT_OK )
		return;
	PClubSetCameraFrame( (const u8*)mBuffer.constData(), mWidth, mHeight, mWidth * 3 );
#endif
}
