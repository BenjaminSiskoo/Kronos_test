/*  UIMovieEditor -- editeur d'entrees du film (outil de TAS).

    Le film charge (enregistrement ou lecture, movie.c) sous forme de
    grille : une ligne par trame, une colonne par bouton des manettes
    numeriques des deux ports, et les octets bruts de chaque port pour les
    autres peripheriques.

    - un clic sur une case appuie / relache le bouton pour cette trame ; en
      gardant le bouton de la souris enfonce, les trames survolees prennent
      le meme etat ;
    - la ligne de la prochaine trame a executer est mise en evidence, les
      trames de lag (le jeu n'a pas lu les manettes) ont leur numero en
      rouge ;
    - modifier une trame deja executee ramene l'emulation en arriere
      (UIYabause::tasSeek() : instantane garde en memoire ou debut du film,
      puis avance rapide) jusqu'a la trame ou l'on etait, ou jusqu'a la
      trame modifiee ;
    - double clic sur un numero de trame : aller a cette trame ;
    - insertion / suppression de trames, remise a zero, copier / coller ;
    - a la fin du film, l'enregistrement reprend avec les manettes.

    Toute modification passe le film en lecture/ecriture ; tant que la
    fenetre est ouverte, charger un etat de sauvegarde ne coupe pas le film.

    This file is part of Yabause (GPL v2 or later). */

#ifndef UIMOVIEEDITOR_H
#define UIMOVIEEDITOR_H

#include <QAbstractTableModel>
#include <QByteArray>
#include <QDialog>
#include <QList>
#include <QString>
#include <QTableView>

class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;
class UIYabause;

// Le film vu comme une table. Les donnees restent dans movie.c.
class MovieModel : public QAbstractTableModel
{
	Q_OBJECT

public:
	explicit MovieModel( QObject* parent = 0 );

	static int columnFrame();
	static int columnButton( int port, int button );
	static int columnRaw( int port );
	static int columns();
	// la colonne est celle d'un bouton : son port et son numero
	static bool isButton( int column, int* port, int* button );
	static bool isRaw( int column, int* port );

	int rowCount( const QModelIndex& parent = QModelIndex() ) const override;
	int columnCount( const QModelIndex& parent = QModelIndex() ) const override;
	QVariant data( const QModelIndex& index, int role = Qt::DisplayRole ) const override;
	QVariant headerData( int section, Qt::Orientation orientation, int role = Qt::DisplayRole ) const override;
	Qt::ItemFlags flags( const QModelIndex& index ) const override;
	bool setData( const QModelIndex& index, const QVariant& value, int role = Qt::EditRole ) override;

	// Appuie / relache un bouton ; false si la case n'est pas modifiable.
	bool setButton( int row, int column, bool pressed );
	// Suit le film : lignes ajoutees / retirees a la fin, trame courante.
	void sync();
	// Le contenu a change ailleurs qu'a la fin.
	void reload();
	int currentFrame() const { return mCurrent; }

signals:
	// l'entree de la trame 'frame' vient d'etre modifiee
	void edited( int frame );

private:
	int mRows;
	int mCurrent;
};

// Grille : clic et glisser sur les cases des boutons.
class MovieView : public QTableView
{
	Q_OBJECT

public:
	explicit MovieView( QWidget* parent = 0 );
	void setEditingEnabled( bool enabled ) { mEnabled = enabled; }

signals:
	// fin d'un clic / glisser sur les boutons
	void paintFinished();

protected:
	void mousePressEvent( QMouseEvent* e ) override;
	void mouseMoveEvent( QMouseEvent* e ) override;
	void mouseReleaseEvent( QMouseEvent* e ) override;

private:
	bool mEnabled;
	bool mPainting;
	bool mPaintValue;
	int mPaintColumn;
	int mLastRow;
};

class UIMovieEditor : public QDialog
{
	Q_OBJECT

public:
	UIMovieEditor( UIYabause* parent );
	~UIMovieEditor();

protected:
	void closeEvent( QCloseEvent* e ) override;

private slots:
	void tick();
	void onEdited( int frame );
	void finishEdit();
	void seekFinished();
	void frameStepped();
	void rowDoubleClicked( const QModelIndex& index );
	void seekToSelection();
	void runPause();
	void insertFrames();
	void deleteFrames();
	void clearFrames();
	void copyFrames();
	void pasteFrames();
	void snapshotsChanged();
	void selectionChanged();

private:
	struct ClipRow
	{
		int size[2];
		QByteArray data[2];
	};

	// lignes selectionnees, triees
	QList<int> selectedRows() const;
	void applySnapshots();
	void updateStatus();
	void endSession();

	UIYabause* mMain;
	MovieModel* mModel;
	MovieView* mView;
	QLabel* mStatus;
	QCheckBox* mFollow;
	QCheckBox* mRestore;
	QCheckBox* mSnapshots;
	QSpinBox* mSnapInterval;
	QPushButton* mRun;
	QPushButton* mStep;
	QPushButton* mSeek;
	QPushButton* mInsert;
	QPushButton* mDelete;
	QPushButton* mClear;
	QPushButton* mCopy;
	QPushButton* mPaste;
	QTimer* mTimer;
	QList<ClipRow> mClipboard;
	// une trame deja executee a ete modifiee : retour en arriere a faire
	bool mPendingSeek;
	int mFirstEdited;
	int mRestoreFrame;
	int mLastCurrent;
	// film affiche (nom du fichier), pour voir qu'un autre a ete demarre
	QString mMovieName;
};

#endif // UIMOVIEEDITOR_H
