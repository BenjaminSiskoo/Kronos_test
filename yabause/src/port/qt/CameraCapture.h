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

/* Webcam of the ST-V Print Club games: while the loaded game has the Print
   Club video board (PClubIsEnabled()), the chosen webcam is opened with
   openpnp-capture and every new picture goes to PClubSetCameraFrame().
   The board shows a test pattern when no picture comes (no camera, camera
   "None", or Kronos built without openpnp-capture: HAVE_OPENPNP_CAPTURE).

   Setting "PrintClub/Camera" (see UISettings):
     ""      automatic: the first camera found
     "none"  no camera (test pattern)
     other   unique ID of the camera (Cap_getDeviceUniqueID()) */

#ifndef CAMERACAPTURE_H
#define CAMERACAPTURE_H

#include <QObject>
#include <QTimer>
#include <QString>
#include <QList>
#include <QByteArray>

class CameraCapture : public QObject
{
	Q_OBJECT

public:
	struct Device
	{
		QString id;    /* Cap_getDeviceUniqueID() */
		QString name;  /* Cap_getDeviceName() */
	};

	explicit CameraCapture( QObject* parent = 0 );
	virtual ~CameraCapture();

	/* Cameras present now. Empty without openpnp-capture. */
	static QList<Device> devices();
	/* false when Kronos was built without openpnp-capture */
	static bool available();

	/* "" = first camera, "none" = no camera, else a unique ID. */
	void setDevice( const QString& id );

private slots:
	void poll();

private:
	void openStream();
	void closeStream();

	void* mContext;     /* CapContext */
	int mStream;        /* CapStream, -1 = closed */
	int mWidth;
	int mHeight;
	QByteArray mBuffer; /* RGB 8:8:8 */
	QString mWanted;
	QString mOpenedId;
	int mRetryTicks;    /* polls to wait before trying to open again */
	bool mWarned;
	QTimer mTimer;
};

#endif // CAMERACAPTURE_H
