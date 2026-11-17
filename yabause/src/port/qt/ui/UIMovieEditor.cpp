/*  UIMovieEditor -- editeur d'entrees du film (outil de TAS), voir
    UIMovieEditor.h.

    This file is part of Yabause (GPL v2 or later). */

#include "UIMovieEditor.h"
#include "UIYabause.h"
#include "../CommonDialogs.h"
#include "../QtYabause.h"
#include "../Settings.h"
#include "../VolatileSettings.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

extern "C" {
#include "movie.h"
#include "yabause.h"
}

namespace {

// Colonnes : trame, boutons du port 1, boutons du port 2, octets des ports.
enum
{
	kColFrame = 0,
	kColButtons = 1,
	kColRaw = kColButtons + 2 * MOVIE_PAD_BUTTONS,
	kColCount = kColRaw + 2
};

const char* const kButtonNames[MOVIE_PAD_BUTTONS] = {
	"Up", "Down", "Left", "Right", "Start", "A", "B", "C", "X", "Y", "Z", "L", "R"
};

// memoire maximale des instantanes, en Mo
const int kSnapshotBudget = 512;

} // namespace

//////////////////////////////////////////////////////////////////////////////
// MovieModel
//////////////////////////////////////////////////////////////////////////////

MovieModel::MovieModel( QObject* parent )
	: QAbstractTableModel( parent ), mRows( 0 ), mCurrent( -1 )
{
	mRows = MovieEditFrameCount();
	mCurrent = IsMovieLoaded() ? MovieGetFrame() : -1;
}

int MovieModel::columnFrame() { return kColFrame; }
int MovieModel::columnButton( int port, int button ) { return kColButtons + port * MOVIE_PAD_BUTTONS + button; }
int MovieModel::columnRaw( int port ) { return kColRaw + port; }
int MovieModel::columns() { return kColCount; }

bool MovieModel::isButton( int column, int* port, int* button )
{
	if ( column < kColButtons || column >= kColRaw )
		return false;
	if ( port )
		*port = ( column - kColButtons ) / MOVIE_PAD_BUTTONS;
	if ( button )
		*button = ( column - kColButtons ) % MOVIE_PAD_BUTTONS;
	return true;
}

bool MovieModel::isRaw( int column, int* port )
{
	if ( column < kColRaw || column >= kColCount )
		return false;
	if ( port )
		*port = column - kColRaw;
	return true;
}

int MovieModel::rowCount( const QModelIndex& parent ) const
{
	return parent.isValid() ? 0 : mRows;
}

int MovieModel::columnCount( const QModelIndex& parent ) const
{
	return parent.isValid() ? 0 : kColCount;
}

QVariant MovieModel::headerData( int section, Qt::Orientation orientation, int role ) const
{
	int port = 0, button = 0;
	if ( orientation != Qt::Horizontal )
		return QVariant();
	if ( role == Qt::DisplayRole )
	{
		if ( section == kColFrame )
			return QtYabause::translate( "Frame" );
		if ( isButton( section, &port, &button ) )
		{
			// lettres des gachettes en majuscules ; le port 2 est suffixe
			const QString letter = QString( QChar( MovieEditButtonLetter( button ) ) ).toUpper();
			return port == 0 ? letter : letter + "2";
		}
		if ( isRaw( section, &port ) )
			return QtYabause::translate( "Port %1 data" ).arg( port + 1 );
	}
	else if ( role == Qt::ToolTipRole )
	{
		if ( isButton( section, &port, &button ) )
			return QtYabause::translate( "Port %1: %2" ).arg( port + 1 ).arg( kButtonNames[button] );
		if ( isRaw( section, &port ) )
			return QtYabause::translate( "Bytes sent by the peripheral of port %1 (hexadecimal)" ).arg( port + 1 );
	}
	return QVariant();
}

QVariant MovieModel::data( const QModelIndex& index, int role ) const
{
	int port = 0, button = 0;
	const int row = index.row();
	const int col = index.column();

	if ( !index.isValid() || row < 0 || row >= mRows )
		return QVariant();

	if ( role == Qt::TextAlignmentRole )
		return (int)( isRaw( col, 0 ) ? ( Qt::AlignLeft | Qt::AlignVCenter ) : Qt::AlignCenter );

	if ( col == kColFrame )
	{
		if ( role == Qt::DisplayRole )
			return row;
		if ( role == Qt::BackgroundRole )
		{
			if ( MovieEditGetLag( row ) == 1 )
				return QBrush( QColor( 225, 110, 110 ) );
			if ( row == mCurrent )
				return QBrush( QColor( 255, 215, 110 ) );
		}
		if ( role == Qt::ForegroundRole && ( MovieEditGetLag( row ) == 1 || row == mCurrent ) )
			return QBrush( Qt::black );
		if ( role == Qt::ToolTipRole && MovieEditGetLag( row ) == 1 )
			return QtYabause::translate( "Lag frame: the game did not read the controllers" );
		return QVariant();
	}

	if ( isButton( col, &port, &button ) )
	{
		const int state = MovieEditGetButton( row, port, button );
		if ( role == Qt::DisplayRole )
			return state == 1 ? QString( QChar( MovieEditButtonLetter( button ) ) ).toUpper() : QString();
		if ( role == Qt::BackgroundRole )
		{
			if ( state == 1 )
				return QBrush( port == 0 ? QColor( 60, 110, 200 ) : QColor( 40, 150, 110 ) );
			if ( row == mCurrent )
				return QBrush( QColor( 255, 215, 110 ) );
			if ( state < 0 )
				return QBrush( QColor( 128, 128, 128, 60 ) ); // pas une manette numerique
		}
		if ( role == Qt::ForegroundRole && state == 1 )
			return QBrush( Qt::white );
		return QVariant();
	}

	if ( isRaw( col, &port ) )
	{
		if ( role == Qt::DisplayRole || role == Qt::EditRole )
		{
			QByteArray bytes( MovieEditPortLen(), 0 );
			const int size = MovieEditGetPort( row, port, (u8*)bytes.data(), bytes.size() );
			if ( size <= 0 )
				return QString();
			return QString::fromLatin1( bytes.left( size ).toHex() ).toUpper();
		}
		if ( role == Qt::BackgroundRole && row == mCurrent )
			return QBrush( QColor( 255, 215, 110 ) );
		if ( role == Qt::ForegroundRole && row == mCurrent )
			return QBrush( Qt::black );
	}
	return QVariant();
}

Qt::ItemFlags MovieModel::flags( const QModelIndex& index ) const
{
	Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
	if ( index.isValid() && isRaw( index.column(), 0 ) && MovieCanEdit() )
		f |= Qt::ItemIsEditable;
	return f;
}

// Octets bruts d'un port, saisis en hexadecimal.
bool MovieModel::setData( const QModelIndex& index, const QVariant& value, int role )
{
	int port = 0;
	if ( !index.isValid() || role != Qt::EditRole || !isRaw( index.column(), &port ) )
		return false;

	QString text = value.toString();
	text.remove( ' ' );
	if ( text.length() % 2 != 0 || text.length() / 2 > MovieEditPortLen() )
		return false;
	for ( int i = 0; i < text.length(); i++ )
		if ( !QString( "0123456789abcdefABCDEF" ).contains( text[i] ) )
			return false;
	const QByteArray bytes = QByteArray::fromHex( text.toLatin1() );

	// les octets au-dela de la taille gardent leur valeur
	QByteArray data( MovieEditPortLen(), 0 );
	if ( MovieEditGetPort( index.row(), port, (u8*)data.data(), data.size() ) < 0 )
		return false;
	for ( int i = 0; i < bytes.size(); i++ )
		data[i] = bytes[i];
	if ( MovieEditSetPort( index.row(), port, bytes.size(), (const u8*)data.constData(), data.size() ) != 0 )
		return false;
	emit dataChanged( this->index( index.row(), 0 ), this->index( index.row(), kColCount - 1 ) );
	emit edited( index.row() );
	return true;
}

bool MovieModel::setButton( int row, int column, bool pressed )
{
	int port = 0, button = 0;
	if ( row < 0 || row >= mRows || !isButton( column, &port, &button ) )
		return false;
	const int state = MovieEditGetButton( row, port, button );
	if ( state < 0 )
		return false;
	if ( ( state == 1 ) == pressed )
		return true;
	if ( MovieEditSetButton( row, port, button, pressed ? 1 : 0 ) != 0 )
		return false;
	emit dataChanged( index( row, column ), index( row, column ) );
	emit dataChanged( index( row, columnRaw( port ) ), index( row, columnRaw( port ) ) );
	emit edited( row );
	return true;
}

void MovieModel::sync()
{
	const int rows = MovieEditFrameCount();
	if ( rows > mRows )
	{
		beginInsertRows( QModelIndex(), mRows, rows - 1 );
		mRows = rows;
		endInsertRows();
	}
	else if ( rows < mRows )
	{
		beginRemoveRows( QModelIndex(), rows, mRows - 1 );
		mRows = rows;
		endRemoveRows();
	}

	const int current = IsMovieLoaded() ? MovieGetFrame() : -1;
	if ( current != mCurrent )
	{
		// la mise en evidence change de ligne, et le lag des trames
		// executees entre les deux vient d'etre mesure
		int first = qMin( mCurrent, current ) - 1;
		int last = qMax( mCurrent, current );
		mCurrent = current;
		if ( first < 0 )
			first = 0;
		if ( last > mRows - 1 )
			last = mRows - 1;
		if ( first <= last )
			emit dataChanged( index( first, 0 ), index( last, kColCount - 1 ) );
	}
}

void MovieModel::reload()
{
	beginResetModel();
	mRows = MovieEditFrameCount();
	mCurrent = IsMovieLoaded() ? MovieGetFrame() : -1;
	endResetModel();
}

//////////////////////////////////////////////////////////////////////////////
// MovieView
//////////////////////////////////////////////////////////////////////////////

MovieView::MovieView( QWidget* parent )
	: QTableView( parent ), mEnabled( true ), mPainting( false ), mPaintValue( false ), mPaintColumn( -1 ), mLastRow( -1 )
{
}

void MovieView::mousePressEvent( QMouseEvent* e )
{
	MovieModel* m = qobject_cast<MovieModel*>( model() );
	const QModelIndex idx = indexAt( e->pos() );
	int port = 0, button = 0;

	if ( m && e->button() == Qt::LeftButton && idx.isValid() && MovieModel::isButton( idx.column(), &port, &button ) )
	{
		// les cases des boutons ne servent pas a la selection
		if ( !mEnabled || !MovieCanEdit() )
			return;
		const int state = MovieEditGetButton( idx.row(), port, button );
		if ( state < 0 )
			return;
		mPainting = true;
		mPaintValue = ( state == 0 );
		mPaintColumn = idx.column();
		mLastRow = idx.row();
		m->setButton( idx.row(), idx.column(), mPaintValue );
		return;
	}
	QTableView::mousePressEvent( e );
}

void MovieView::mouseMoveEvent( QMouseEvent* e )
{
	if ( !mPainting )
	{
		QTableView::mouseMoveEvent( e );
		return;
	}
	MovieModel* m = qobject_cast<MovieModel*>( model() );
	// meme colonne, quelle que soit la position horizontale de la souris
	const int row = rowAt( e->pos().y() );
	if ( !m || row < 0 || row == mLastRow )
		return;
	const int step = ( row > mLastRow ) ? 1 : -1;
	for ( int r = mLastRow + step; r != row + step; r += step )
		m->setButton( r, mPaintColumn, mPaintValue );
	mLastRow = row;
}

void MovieView::mouseReleaseEvent( QMouseEvent* e )
{
	if ( !mPainting )
	{
		QTableView::mouseReleaseEvent( e );
		return;
	}
	mPainting = false;
	emit paintFinished();
}

//////////////////////////////////////////////////////////////////////////////
// UIMovieEditor
//////////////////////////////////////////////////////////////////////////////

UIMovieEditor::UIMovieEditor( UIYabause* parent )
	: QDialog( parent ), mMain( parent ), mPendingSeek( false ), mFirstEdited( 0 ), mRestoreFrame( 0 ), mLastCurrent( -1 )
{
	VolatileSettings* vs = QtYabause::volatileSettings();

	setWindowTitle( QtYabause::translate( "Input editor" ) );
	setWindowFlags( ( windowFlags() & ~Qt::WindowContextHelpButtonHint ) | Qt::WindowMaximizeButtonHint );
	setModal( false );
	resize( 900, 560 );

	mModel = new MovieModel( this );
	mView = new MovieView( this );
	mView->setModel( mModel );
	mView->setSelectionBehavior( QAbstractItemView::SelectRows );
	mView->setSelectionMode( QAbstractItemView::ExtendedSelection );
	mView->setEditTriggers( QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed );
	mView->setVerticalScrollMode( QAbstractItemView::ScrollPerItem );
	mView->verticalHeader()->setVisible( false );
	mView->verticalHeader()->setSectionResizeMode( QHeaderView::Fixed );
	mView->verticalHeader()->setDefaultSectionSize( mView->fontMetrics().height() + 4 );
	mView->horizontalHeader()->setSectionResizeMode( QHeaderView::Fixed );
	mView->horizontalHeader()->setHighlightSections( false );
	{
		// boundingRect() : horizontalAdvance() n'existe que depuis Qt 5.11
		const int w = mView->fontMetrics().boundingRect( "MM" ).width() + 8;
		mView->setColumnWidth( MovieModel::columnFrame(), mView->fontMetrics().boundingRect( "00000000" ).width() + 12 );
		for ( int port = 0; port < 2; port++ )
		{
			for ( int b = 0; b < MOVIE_PAD_BUTTONS; b++ )
				mView->setColumnWidth( MovieModel::columnButton( port, b ), w );
			mView->horizontalHeader()->setSectionResizeMode( MovieModel::columnRaw( port ), QHeaderView::Interactive );
			mView->setColumnWidth( MovieModel::columnRaw( port ), 150 );
		}
	}

	mStatus = new QLabel( this );
	mStatus->setTextInteractionFlags( Qt::TextSelectableByMouse );

	mRun = new QPushButton( this );
	mStep = new QPushButton( QtYabause::translate( "Frame advance" ), this );
	mSeek = new QPushButton( QtYabause::translate( "Go to selected frame" ), this );
	mInsert = new QPushButton( QtYabause::translate( "Insert frames..." ), this );
	mDelete = new QPushButton( QtYabause::translate( "Delete frames" ), this );
	mClear = new QPushButton( QtYabause::translate( "Release buttons" ), this );
	mCopy = new QPushButton( QtYabause::translate( "Copy" ), this );
	mPaste = new QPushButton( QtYabause::translate( "Paste" ), this );
	mInsert->setToolTip( QtYabause::translate( "Insert empty frames before the first selected frame, or at the end of the movie when nothing is selected" ) );
	mPaste->setToolTip( QtYabause::translate( "Replace the inputs from the first selected frame with the copied frames" ) );
	foreach ( QPushButton* b, QList<QPushButton*>() << mRun << mStep << mSeek << mInsert << mDelete << mClear << mCopy << mPaste )
		b->setAutoDefault( false );

	mFollow = new QCheckBox( QtYabause::translate( "Follow the current frame" ), this );
	mFollow->setChecked( vs->value( "TAS/EditorFollow", true ).toBool() );
	mRestore = new QCheckBox( QtYabause::translate( "After an edit, come back to the frame I was on" ), this );
	mRestore->setChecked( vs->value( "TAS/EditorRestore", true ).toBool() );
	mRestore->setToolTip( QtYabause::translate( "Unchecked: the emulation stops on the edited frame" ) );
	mSnapshots = new QCheckBox( QtYabause::translate( "Keep a snapshot every" ), this );
	mSnapshots->setChecked( vs->value( "TAS/EditorSnapshots", true ).toBool() );
	mSnapshots->setToolTip( QtYabause::translate( "Going back starts from the nearest snapshot instead of the beginning of the movie. Uncheck it if a replay from a snapshot desynchronises." ) );
	mSnapInterval = new QSpinBox( this );
	mSnapInterval->setRange( 30, 36000 );
	mSnapInterval->setSingleStep( 30 );
	mSnapInterval->setSuffix( QtYabause::translate( " frames" ) );
	mSnapInterval->setValue( vs->value( "TAS/EditorSnapshotInterval", 300 ).toInt() );

	QHBoxLayout* transport = new QHBoxLayout;
	transport->addWidget( mRun );
	transport->addWidget( mStep );
	transport->addWidget( mSeek );
	transport->addStretch( 1 );
	transport->addWidget( mInsert );
	transport->addWidget( mDelete );
	transport->addWidget( mClear );
	transport->addWidget( mCopy );
	transport->addWidget( mPaste );

	QHBoxLayout* options = new QHBoxLayout;
	options->addWidget( mFollow );
	options->addWidget( mRestore );
	options->addStretch( 1 );
	options->addWidget( mSnapshots );
	options->addWidget( mSnapInterval );

	QVBoxLayout* all = new QVBoxLayout( this );
	all->addWidget( mStatus );
	all->addWidget( mView, 1 );
	all->addLayout( transport );
	all->addLayout( options );

	connect( mModel, SIGNAL( edited( int ) ), this, SLOT( onEdited( int ) ) );
	connect( mView, SIGNAL( paintFinished() ), this, SLOT( finishEdit() ) );
	connect( mView, SIGNAL( doubleClicked( const QModelIndex& ) ), this, SLOT( rowDoubleClicked( const QModelIndex& ) ) );
	connect( mView->selectionModel(), SIGNAL( selectionChanged( const QItemSelection&, const QItemSelection& ) ), this, SLOT( selectionChanged() ) );
	connect( mRun, SIGNAL( clicked() ), this, SLOT( runPause() ) );
	connect( mStep, SIGNAL( clicked() ), mMain, SLOT( tasFrameAdvance() ) );
	connect( mSeek, SIGNAL( clicked() ), this, SLOT( seekToSelection() ) );
	connect( mInsert, SIGNAL( clicked() ), this, SLOT( insertFrames() ) );
	connect( mDelete, SIGNAL( clicked() ), this, SLOT( deleteFrames() ) );
	connect( mClear, SIGNAL( clicked() ), this, SLOT( clearFrames() ) );
	connect( mCopy, SIGNAL( clicked() ), this, SLOT( copyFrames() ) );
	connect( mPaste, SIGNAL( clicked() ), this, SLOT( pasteFrames() ) );
	connect( mSnapshots, SIGNAL( toggled( bool ) ), this, SLOT( snapshotsChanged() ) );
	connect( mSnapInterval, SIGNAL( valueChanged( int ) ), this, SLOT( snapshotsChanged() ) );
	connect( mMain, SIGNAL( tasSeekFinished() ), this, SLOT( seekFinished() ) );
	connect( mMain, SIGNAL( tasFrameStepped() ), this, SLOT( frameStepped() ) );

	// Tant que l'editeur est ouvert, charger un etat ne coupe pas le film.
	MovieSetKeepOnLoad( 1 );
	applySnapshots();

	mTimer = new QTimer( this );
	connect( mTimer, SIGNAL( timeout() ), this, SLOT( tick() ) );
	mTimer->start( 50 );
	tick();
	selectionChanged();
}

UIMovieEditor::~UIMovieEditor()
{
	endSession();
}

void UIMovieEditor::endSession()
{
	MovieSetKeepOnLoad( 0 );
	// libere la memoire des instantanes
	MovieSetSnapshots( 0, kSnapshotBudget );
}

void UIMovieEditor::closeEvent( QCloseEvent* e )
{
	Settings* s = QtYabause::settings();
	s->setValue( "TAS/EditorFollow", mFollow->isChecked() );
	s->setValue( "TAS/EditorRestore", mRestore->isChecked() );
	s->setValue( "TAS/EditorSnapshots", mSnapshots->isChecked() );
	s->setValue( "TAS/EditorSnapshotInterval", mSnapInterval->value() );
	endSession();
	QDialog::closeEvent( e );
}

void UIMovieEditor::applySnapshots()
{
	MovieSetSnapshots( mSnapshots->isChecked() ? mSnapInterval->value() : 0, kSnapshotBudget );
}

void UIMovieEditor::snapshotsChanged()
{
	mSnapInterval->setEnabled( mSnapshots->isChecked() );
	applySnapshots();
}

QList<int> UIMovieEditor::selectedRows() const
{
	QList<int> rows;
	foreach ( const QModelIndex& index, mView->selectionModel()->selectedRows() )
		rows.append( index.row() );
	std::sort( rows.begin(), rows.end() );
	return rows;
}

void UIMovieEditor::selectionChanged()
{
	const int count = mView->selectionModel()->selectedRows().size();
	const bool canEdit = MovieCanEdit() && !mMain->tasSeeking();
	mSeek->setEnabled( count == 1 && IsMovieLoaded() && !mMain->tasSeeking() );
	mInsert->setEnabled( canEdit );
	mDelete->setEnabled( canEdit && count > 0 );
	mClear->setEnabled( canEdit && count > 0 );
	mCopy->setEnabled( count > 0 );
	mPaste->setEnabled( canEdit && count > 0 && !mClipboard.isEmpty() );
}

void UIMovieEditor::updateStatus()
{
	QString text;
	if ( !IsMovieLoaded() )
		text = QtYabause::translate( "No movie: record or play an input movie (Tools > TAS / Input movie) to edit it." );
	else
	{
		text = ( Movie.Status == Recording )
			? QtYabause::translate( "Recording - frame %1" ).arg( MovieGetFrame() )
			: QtYabause::translate( "Playback - frame %1 / %2" ).arg( MovieGetFrame() ).arg( MovieGetLength() );
		text += QtYabause::translate( "   lag %1   re-records %2   snapshots %3" )
			.arg( MovieGetLagCount() ).arg( MovieGetRerecords() ).arg( MovieGetSnapshotCount() );
		if ( !MovieCanEdit() )
			text += "   " + QtYabause::translate( "(the movie file can't be written: read-only)" );
		if ( mMain->tasSeeking() )
			text += "   " + QtYabause::translate( "Going back..." );
		else if ( MovieIsStale() )
			text += "   " + QtYabause::translate( "Edited behind the current frame" );
		if ( MovieGetDesyncFrame() >= 0 )
			text += "   " + QtYabause::translate( "DESYNC at frame %1" ).arg( MovieGetDesyncFrame() );
	}
	if ( text != mStatus->text() )
		mStatus->setText( text );

	const QString run = mMain->mYabauseThread->emulationPaused()
		? QtYabause::translate( "Run" ) : QtYabause::translate( "Pause" );
	if ( run != mRun->text() )
		mRun->setText( run );
}

void UIMovieEditor::tick()
{
	// un autre film a ete demarre (ou le film arrete) : tout le contenu change
	const QString name = IsMovieLoaded() ? QString::fromLocal8Bit( Movie.filename ? Movie.filename : "" ) + QChar( '*' ) : QString();
	if ( name != mMovieName )
	{
		mMovieName = name;
		mPendingSeek = false;
		mView->clearSelection();
		mModel->reload();
	}
	mModel->sync();
	mView->setEditingEnabled( !mMain->tasSeeking() );

	const int current = mModel->currentFrame();
	if ( current != mLastCurrent )
	{
		mLastCurrent = current;
		if ( mFollow->isChecked() && current >= 0 && mModel->rowCount() > 0 )
			mView->scrollTo( mModel->index( qMin( current, mModel->rowCount() - 1 ), 0 ), QAbstractItemView::EnsureVisible );
	}
	updateStatus();
	selectionChanged();
}

void UIMovieEditor::frameStepped()
{
	tick();
}

// Une entree vient de changer. Si la trame a deja ete executee, il faudra
// revenir en arriere ; on attend la fin du geste (glisser sur plusieurs
// trames, collage...) pour le faire une seule fois.
void UIMovieEditor::onEdited( int frame )
{
	if ( !MovieIsStale() )
		return;
	if ( !mPendingSeek )
	{
		mPendingSeek = true;
		mFirstEdited = frame;
		mRestoreFrame = MovieGetFrame();
	}
	else if ( frame < mFirstEdited )
		mFirstEdited = frame;
}

void UIMovieEditor::finishEdit()
{
	if ( !mPendingSeek )
	{
		tick();
		return;
	}
	mPendingSeek = false;
	const int target = mRestore->isChecked() ? mRestoreFrame : mFirstEdited;
	if ( !mMain->tasSeek( target, false ) )
		CommonDialogs::error( QtYabause::translate( "Couldn't go back in the movie." ) );
	tick();
}

void UIMovieEditor::seekFinished()
{
	mModel->sync();
	mView->viewport()->update();
	tick();
}

void UIMovieEditor::rowDoubleClicked( const QModelIndex& index )
{
	// les colonnes d'octets s'editent au double clic
	if ( !index.isValid() || index.column() != MovieModel::columnFrame() )
		return;
	if ( IsMovieLoaded() && !mMain->tasSeeking() )
		mMain->tasSeek( index.row(), false );
	tick();
}

void UIMovieEditor::seekToSelection()
{
	const QList<int> rows = selectedRows();
	if ( rows.size() != 1 || !IsMovieLoaded() || mMain->tasSeeking() )
		return;
	mMain->tasSeek( rows[0], false );
	tick();
}

void UIMovieEditor::runPause()
{
	if ( mMain->mYabauseThread->emulationPaused() )
		mMain->aEmulationRun->trigger();
	else
		mMain->aEmulationPause->trigger();
	tick();
}

void UIMovieEditor::insertFrames()
{
	if ( !MovieCanEdit() || mMain->tasSeeking() )
		return;
	const QList<int> rows = selectedRows();
	const int at = rows.isEmpty() ? MovieEditFrameCount() : rows.first();
	bool ok = false;
	const int count = QInputDialog::getInt( this, QtYabause::translate( "Insert frames" ),
		rows.isEmpty() ? QtYabause::translate( "Number of empty frames to add at the end of the movie:" )
		               : QtYabause::translate( "Number of empty frames to insert before frame %1:" ).arg( at ),
		1, 1, 100000, 1, &ok );
	if ( !ok )
		return;
	const int before = MovieGetFrame();
	if ( MovieEditInsert( at, count ) != 0 )
	{
		CommonDialogs::error( QtYabause::translate( "Couldn't modify the movie." ) );
		mModel->reload();
		return;
	}
	mModel->reload();
	if ( MovieIsStale() )
	{
		mPendingSeek = true;
		mFirstEdited = at;
		// la trame ou l'on etait a recule de 'count' lignes
		mRestoreFrame = before + count;
	}
	finishEdit();
}

void UIMovieEditor::deleteFrames()
{
	if ( !MovieCanEdit() || mMain->tasSeeking() )
		return;
	const QList<int> rows = selectedRows();
	if ( rows.isEmpty() )
		return;
	const int before = MovieGetFrame();
	int removedBefore = 0;
	bool failed = false;

	// par blocs de lignes contigues, du bas vers le haut
	int end = rows.size() - 1;
	while ( end >= 0 && !failed )
	{
		int start = end;
		while ( start > 0 && rows[start - 1] == rows[start] - 1 )
			start--;
		const int at = rows[start];
		const int count = rows[end] - rows[start] + 1;
		if ( MovieEditDelete( at, count ) != 0 )
			failed = true;
		else if ( at < before )
			removedBefore += qMin( count, before - at );
		end = start - 1;
	}
	mView->clearSelection();
	mModel->reload();
	if ( failed )
		CommonDialogs::error( QtYabause::translate( "Couldn't modify the movie." ) );
	if ( MovieIsStale() )
	{
		mPendingSeek = true;
		mFirstEdited = rows.first();
		mRestoreFrame = before - removedBefore;
	}
	finishEdit();
}

void UIMovieEditor::clearFrames()
{
	if ( !MovieCanEdit() || mMain->tasSeeking() )
		return;
	foreach ( int row, selectedRows() )
		for ( int port = 0; port < 2; port++ )
			for ( int b = 0; b < MOVIE_PAD_BUTTONS; b++ )
				mModel->setButton( row, MovieModel::columnButton( port, b ), false );
	finishEdit();
}

void UIMovieEditor::copyFrames()
{
	mClipboard.clear();
	foreach ( int row, selectedRows() )
	{
		ClipRow clip;
		for ( int port = 0; port < 2; port++ )
		{
			clip.data[port] = QByteArray( MovieEditPortLen(), 0 );
			clip.size[port] = MovieEditGetPort( row, port, (u8*)clip.data[port].data(), clip.data[port].size() );
		}
		if ( clip.size[0] >= 0 && clip.size[1] >= 0 )
			mClipboard.append( clip );
	}
	selectionChanged();
}

void UIMovieEditor::pasteFrames()
{
	if ( !MovieCanEdit() || mMain->tasSeeking() || mClipboard.isEmpty() )
		return;
	const QList<int> rows = selectedRows();
	if ( rows.isEmpty() )
		return;
	const int at = rows.first();
	const int portLen = MovieEditPortLen();
	bool failed = false;

	for ( int i = 0; i < mClipboard.size() && at + i < MovieEditFrameCount(); i++ )
	{
		for ( int port = 0; port < 2; port++ )
		{
			// le film copie pouvait avoir des ports plus longs ou plus courts
			QByteArray data = mClipboard[i].data[port].left( portLen );
			data.append( QByteArray( portLen - data.size(), 0 ) );
			if ( MovieEditSetPort( at + i, port, qMin( mClipboard[i].size[port], portLen ),
				(const u8*)data.constData(), data.size() ) != 0 )
				failed = true;
		}
		onEdited( at + i );
	}
	mModel->reload();
	if ( failed )
		CommonDialogs::error( QtYabause::translate( "Couldn't modify the movie." ) );
	finishEdit();
}
