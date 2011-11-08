/*  smplayer2, GUI front-end for mplayer2.
    Copyright (C) 2006-2010 Ricardo Villalba <rvm@escomposlinux.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "videopreview.h"
#include "videopreviewconfigdialog.h"
#include <QProcess>
#include <QRegExp>
#include <QDir>
#include <QTime>
#include <QProgressDialog>
#include <QWidget>
#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QPainter>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QPixmapCache>
#include <QImageWriter>
#include <QImageReader>

#include <cmath>

#define RENAME_PICTURES 0

VideoPreview::VideoPreview(QString mplayer_path, QWidget * parent) : QWidget(parent, Qt::Window)
{
	setMplayerPath(mplayer_path);

	set = 0; // settings
	save_last_directory = true;

	prop.input_video.clear();
	prop.dvd_device.clear();
	prop.n_cols = 4;
	prop.n_rows = 4;
	prop.initial_step = 20;
	prop.max_width = 800;
	prop.aspect_ratio = 0;
	prop.display_osd = true;
	prop.extract_format = JPEG;

	output_dir = "smplayer2_preview";
	full_output_dir = QDir::tempPath() +"/"+ output_dir;

	progress = new QProgressDialog(this);
	progress->setMinimumDuration(0);
	connect( progress, SIGNAL(canceled()), this, SLOT(cancelPressed()) );

	w_contents = new QWidget(this);
	QPalette p = w_contents->palette();
	p.setColor(w_contents->backgroundRole(), Qt::white);
	p.setColor(w_contents->foregroundRole(), Qt::black);
	w_contents->setPalette(p);

	info = new QLabel(this);

	foot = new QLabel(this);
	foot->setAlignment(Qt::AlignRight);

	grid_layout = new QGridLayout;
	grid_layout->setSpacing(2);

	QVBoxLayout * l = new QVBoxLayout;
	l->setSizeConstraint(QLayout::SetFixedSize);
	l->addWidget(info);
	l->addLayout(grid_layout);
	l->addWidget(foot);
	
	w_contents->setLayout(l);

	scroll_area = new QScrollArea(this);
	scroll_area->setWidgetResizable(true);
	scroll_area->setAlignment(Qt::AlignCenter);
	scroll_area->setWidget( w_contents );

	button_box = new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::Save, Qt::Horizontal, this);
	connect( button_box, SIGNAL(rejected()), this, SLOT(close()) );
	connect( button_box->button(QDialogButtonBox::Save), SIGNAL(clicked()), this, SLOT(saveImage()) );

	QVBoxLayout * my_layout = new QVBoxLayout;
	my_layout->addWidget(scroll_area);
	my_layout->addWidget(button_box);
	setLayout(my_layout);

	retranslateStrings();

	QList<QByteArray> r_formats = QImageReader::supportedImageFormats();
	QString read_formats;
	for (int n=0; n < r_formats.count(); n++) {
		read_formats.append(r_formats[n]+" ");
	}
	qDebug("VideoPreview::VideoPreview: supported formats for reading: %s", read_formats.toUtf8().constData());

	QList<QByteArray> w_formats = QImageWriter::supportedImageFormats();
	QString write_formats;
	for (int n=0; n < w_formats.count(); n++) {
		write_formats.append(w_formats[n]+" ");
	}
	qDebug("VideoPreview::VideoPreview: supported formats for writing: %s", write_formats.toUtf8().constData());

	toggleInfoAct = new QAction(this);
	toggleInfoAct->setCheckable(true);
	toggleInfoAct->setChecked(true);
	toggleInfoAct->setShortcut( QKeySequence("Ctrl+H") );
	connect( toggleInfoAct, SIGNAL(toggled(bool)), this, SLOT(showInfo(bool)) );
	addAction(toggleInfoAct);
}

VideoPreview::~VideoPreview() {
	if (set) saveSettings();
}

void VideoPreview::retranslateStrings() {
	progress->setWindowTitle(tr("Video preview"));
	progress->setCancelButtonText( tr("Cancel") );

	foot->setText("<i>"+ tr("Generated by SMPlayer2") +" </i>");
}

void VideoPreview::setMplayerPath(QString mplayer_path) {
	mplayer_bin = mplayer_path;
	QFileInfo fi(mplayer_bin);
	if (fi.exists() && fi.isExecutable() && !fi.isDir()) {
		mplayer_bin = fi.absoluteFilePath();
    }

	qDebug("VideoPreview::setMplayerPath: mplayer_bin: '%s'", mplayer_bin.toUtf8().constData());
}

void VideoPreview::setSettings(QSettings * settings) {
	set = settings;
	loadSettings();
}

void VideoPreview::clearThumbnails() {
	for (int n=0; n < label_list.count(); n++) {
		grid_layout->removeWidget( label_list[n] );
		delete label_list[n];
	}
	label_list.clear();
	info->clear();
}

QString VideoPreview::framePicture() {
	if (prop.extract_format == PNG)
		return "00000005.png";
	else
		return "00000005.jpg";
}

bool VideoPreview::createThumbnails() {
	clearThumbnails();
	error_message.clear();

	button_box->setEnabled(false);

	bool result = extractImages();

	progress->close();

	if ((result == false) && (!error_message.isEmpty())) {
		QMessageBox::critical(this, tr("Error"), 
                              tr("The following error has occurred while creating the thumbnails:")+"\n"+ error_message );
	}

	button_box->setEnabled(true);

	// Adjust size
	//resize( w_contents->sizeHint() );

	cleanDir(full_output_dir);
	return result;
}

bool VideoPreview::extractImages() {
	VideoInfo i = getInfo(mplayer_bin, prop.input_video);
	int length = i.length;

	if (length == 0) {
		if (error_message.isEmpty()) error_message = tr("The length of the video is 0");
		return false;
	}

	// Create a temporary directory
	QDir d(QDir::tempPath());
	if (!d.exists(output_dir)) {
		if (!d.mkpath(output_dir)) {
			qDebug("VideoPreview::extractImages: error: can't create '%s'", full_output_dir.toUtf8().constData());
			error_message = tr("The temporary directory (%1) can't be created").arg(full_output_dir);
			return false;
		}
	}

	displayVideoInfo(i);

	// Let's begin
	run.thumbnail_width = 0;

	int num_pictures = prop.n_cols * prop.n_rows;
	length -= prop.initial_step;
	int s_step = length / num_pictures;

	int current_time = prop.initial_step;

	canceled = false;
	progress->setLabelText(tr("Creating thumbnails..."));
	progress->setRange(0, num_pictures-1);
	progress->show();

	double aspect_ratio = i.aspect;
	if (prop.aspect_ratio != 0) aspect_ratio = prop.aspect_ratio;

	for (int n = 0; n < num_pictures; n++) {
		qDebug("VideoPreview::extractImages: getting frame %d of %d...", n+1, num_pictures);
		progress->setValue(n);
		qApp->processEvents();

		if (canceled) return false;

		if (!runMplayer(current_time, aspect_ratio)) return false;

		QString frame_picture = full_output_dir + "/" + framePicture();
		if (!QFile::exists(frame_picture)) {
			error_message = tr("The file %1 doesn't exist").arg(frame_picture);
			return false;
		}

#if RENAME_PICTURES 
		QString extension = (extractFormat()==PNG) ? "png" : "jpg";
		QString output_file = output_dir + QString("/picture_%1.%2").arg(current_time, 8, 10, QLatin1Char('0')).arg(extension);
		d.rename(output_dir + "/" + framePicture(), output_file);
#else
		QString output_file = output_dir + "/" + framePicture();
#endif

		if (!addPicture(QDir::tempPath() +"/"+ output_file, n, current_time)) {
			return false;
		}

		current_time += s_step;
	}

	return true;
}

bool VideoPreview::runMplayer(int seek, double aspect_ratio) {
	QStringList args;
	args << "-nosound";

	if (prop.extract_format == PNG) {
		args << "-vo"
		<< "png:outdir=\""+full_output_dir+"\"";
	} else {
		args << "-vo"
		<< "jpeg:outdir=\""+full_output_dir+"\"";
	}

	args << "-frames" << "6" << "-ss" << QString::number(seek);

	if (aspect_ratio != 0) {
		args << "-aspect" << QString::number(aspect_ratio) << "-zoom";
	}

	if (!prop.dvd_device.isEmpty()) {
		args << "-dvd-device" << prop.dvd_device;
	}

	/*
	if (display_osd) {
		args << "-vf" << "expand=osd=1" << "-osdlevel" << "2";
	}
	*/

	args << prop.input_video;

	QString command = mplayer_bin + " ";
	for (int n = 0; n < args.count(); n++) command = command + args[n] + " ";
	qDebug("VideoPreview::runMplayer: command: %s", command.toUtf8().constData());

	QProcess p;
	p.start(mplayer_bin, args);
	if (!p.waitForFinished()) {
		qDebug("VideoPreview::runMplayer: error running process");
		error_message = tr("The mplayer process didn't run");
		return false;
	}

	return true;
}


bool VideoPreview::addPicture(const QString & filename, int num, int time) {
	int row = num / prop.n_cols;
	int col = num % prop.n_cols;

	qDebug("VideoPreview::addPicture: %d (row: %d col: %d) file: '%s'", num, row, col, filename.toUtf8().constData());

	QPixmapCache::clear();
	QPixmap picture;
	if (!picture.load(filename)) {
		qDebug("VideoPreview::addPicture: can't load file");
		error_message = tr("The file %1 can't be loaded").arg(filename);
		return false;
	}

	if (run.thumbnail_width == 0) {
		int spacing = grid_layout->horizontalSpacing() * (prop.n_cols-1);
		if (spacing < 0) spacing = 0;
		qDebug("VideoPreview::addPicture: spacing: %d", spacing);
		run.thumbnail_width = (prop.max_width - spacing) / prop.n_cols;
		if (run.thumbnail_width > picture.width()) run.thumbnail_width = picture.width();
		qDebug("VideoPreview::addPicture: thumbnail_width set to %d", run.thumbnail_width);
	}

	QPixmap scaled_picture = picture.scaledToWidth(run.thumbnail_width, Qt::SmoothTransformation);

	// Add current time text
	if (prop.display_osd) {
		QString stime = QTime().addSecs(time).toString("hh:mm:ss");
		QFont font("Arial");
		font.setBold(true);
		QPainter painter(&scaled_picture);
		painter.setPen( Qt::white );
		painter.setFont(font);
		painter.drawText(scaled_picture.rect(), Qt::AlignRight | Qt::AlignBottom, stime);
	}

	QLabel * l = new QLabel(this);
	label_list.append(l);
	l->setPixmap(scaled_picture);
	//l->setPixmap(picture);
	grid_layout->addWidget(l, row, col);

	return true;
}

void VideoPreview::displayVideoInfo(const VideoInfo & i) {
	// Display info about the video
	QTime t = QTime().addSecs(i.length);

	QString aspect = QString::number(i.aspect);
	if (fabs(1.77 - i.aspect) < 0.1) aspect = "16:9";
	else
	if (fabs(1.33 - i.aspect) < 0.1) aspect = "4:3";
	else
	if (fabs(2.35 - i.aspect) < 0.1) aspect = "2.35:1";

	QString no_info = tr("No info");

	QString fps = (i.fps==0 || i.fps==1000) ? no_info : QString("%1").arg(i.fps);
	QString video_bitrate = (i.video_bitrate==0) ? no_info : tr("%1 kbps").arg(i.video_bitrate/1000);
	QString audio_bitrate = (i.audio_bitrate==0) ? no_info : tr("%1 kbps").arg(i.audio_bitrate/1000);
	QString audio_rate = (i.audio_rate==0) ? no_info : tr("%1 Hz").arg(i.audio_rate);

	info->setText(
		"<b><font size=+1>" + i.filename +"</font></b>"
		"<table cellspacing=4 cellpadding=4><tr>"
		"<td>" +
		tr("Size: %1 MB").arg(i.size / (1024*1024)) + "<br>" +
		tr("Resolution: %1x%2").arg(i.width).arg(i.height) + "<br>" +
		tr("Length: %1").arg(t.toString("hh:mm:ss")) +
		"</td>"
		"<td>" +
		tr("Video format: %1").arg(i.video_format) + "<br>" +
		tr("Frames per second: %1").arg(fps) + "<br>" +
		tr("Aspect ratio: %1").arg(aspect) + //"<br>" +
		"</td>"
		"<td>" +
		tr("Video bitrate: %1").arg(video_bitrate) + "<br>" +
		tr("Audio bitrate: %1").arg(audio_bitrate) + "<br>" +
		tr("Audio rate: %1").arg(audio_rate) + //"<br>" +
		"</td>"
		"</tr></table>" 
	);
	setWindowTitle( tr("Video preview") + " - " + i.filename );
}

void VideoPreview::cleanDir(QString directory) {
	QStringList filter;
	if (prop.extract_format == PNG) {
		filter.append("*.png");
	} else {
		filter.append("*.jpg");
	}

	QDir d(directory);
	QStringList l = d.entryList( filter, QDir::Files, QDir::Unsorted);

	for (int n = 0; n < l.count(); n++) {
		qDebug("VideoPreview::cleanDir: deleting '%s'", l[n].toUtf8().constData());
		d.remove(l[n]);
	}
	qDebug("VideoPreview::cleanDir: removing directory '%s'", directory.toUtf8().constData());
	d.rmpath(directory);
}

VideoInfo VideoPreview::getInfo(const QString & mplayer_path, const QString & filename) {
	VideoInfo i;

	if (filename.isEmpty()) {
		error_message = tr("No filename");
		return i;
	}

	QFileInfo fi(filename);
	if (fi.exists()) {
		i.filename = fi.fileName();
		i.size = fi.size();
	}

	QRegExp rx("^ID_(.*)=(.*)");

	QProcess p;
	p.setProcessChannelMode( QProcess::MergedChannels );

	QStringList args;
	args << "-vo" << "null" << "-ao" << "null" << "-frames" << "1" << "-identify" << "-nocache" << "-noquiet" << filename;

	if (!prop.dvd_device.isEmpty()) {
		args << "-dvd-device" << prop.dvd_device;
	}

	p.start(mplayer_path, args);

	if (p.waitForFinished()) {
		QByteArray line;
		while (p.canReadLine()) {
			line = p.readLine().trimmed();
			qDebug("VideoPreview::getInfo: '%s'", line.constData());
			if (rx.indexIn(line) > -1) {
				QString tag = rx.cap(1);
				QString value = rx.cap(2);
				qDebug("VideoPreview::getInfo: tag: '%s', value: '%s'", tag.toUtf8().constData(), value.toUtf8().constData());

				if (tag == "LENGTH") i.length = (int) value.toDouble();
				else
				if (tag == "VIDEO_WIDTH") i.width = value.toInt();
				else
				if (tag == "VIDEO_HEIGHT") i.height = value.toInt();
				else
				if (tag == "VIDEO_FPS") i.fps = value.toDouble();
				else
				if (tag == "VIDEO_ASPECT") {
					i.aspect = value.toDouble();
					if ((i.aspect == 0) && (i.width != 0) && (i.height != 0)) {
						i.aspect = (double) i.width / i.height;
					}
				}
				else
				if (tag == "VIDEO_BITRATE") i.video_bitrate = value.toInt();
				else
				if (tag == "AUDIO_BITRATE") i.audio_bitrate = value.toInt();
				else
				if (tag == "AUDIO_RATE") i.audio_rate = value.toInt();
				else
				if (tag == "VIDEO_FORMAT") i.video_format = value;
			}
		}
	} else {
		qDebug("VideoPreview::getInfo: error: process didn't start");
		error_message = tr("The mplayer process didn't start while trying to get info about the video");
	}

	qDebug("VideoPreview::getInfo: filename: '%s'", i.filename.toUtf8().constData());
	qDebug("VideoPreview::getInfo: resolution: '%d x %d'", i.width, i.height);
	qDebug("VideoPreview::getInfo: length: '%d'", i.length);
	qDebug("VideoPreview::getInfo: size: '%d'", (int) i.size);

	return i;
}

void VideoPreview::showInfo(bool visible) {
	qDebug("VideoPreview::showInfo: %d", visible);
	info->setShown(visible);
	foot->setShown(visible);
}

void VideoPreview::saveImage() {
	qDebug("VideoPreview::saveImage");

	// Proposed name
	QString proposed_name = "";
	if (save_last_directory) proposed_name = last_directory;

	QFileInfo fi(prop.input_video);
	if (fi.exists()) {
		if (!save_last_directory) proposed_name = fi.absolutePath();
		QString extension = (extractFormat()==PNG) ? "png" : "jpg";
		proposed_name += "/"+ fi.completeBaseName() +"_preview."+ extension;
	}

	// Formats
	QList<QByteArray> w_formats = QImageWriter::supportedImageFormats();
	QString write_formats;
	for (int n=0; n < w_formats.count(); n++) {
		write_formats.append("*."+w_formats[n]+" ");
	}
	if (write_formats.isEmpty()) {
		// Shouldn't happen!
		write_formats = "*.png *.jpg";
	}

	QString filename = QFileDialog::getSaveFileName(this, tr("Save file"),
                            proposed_name, tr("Images") +" ("+ write_formats +")");

	if (!filename.isEmpty()) {
		QPixmap image = QPixmap::grabWidget(w_contents);
		if (!image.save(filename)) {
			// Failed!!!
			qDebug("VideoPreview::saveImage: error saving '%s'", filename.toUtf8().constData());
			QMessageBox::warning(this, tr("Error saving file"), 
                                 tr("The file couldn't be saved") );
		} else {
			last_directory = QFileInfo(filename).absolutePath();
		}
	}
}

bool VideoPreview::showConfigDialog(QWidget * parent) {
	VideoPreviewConfigDialog d(parent);

	d.setVideoFile( videoFile() );
	d.setDVDDevice( DVDDevice() );
	d.setCols( cols() );
	d.setRows( rows() );
	d.setInitialStep( initialStep() );
	d.setMaxWidth( maxWidth() );
	d.setDisplayOSD( displayOSD() );
	d.setAspectRatio( aspectRatio() );
	d.setFormat( extractFormat() );
	d.setSaveLastDirectory( save_last_directory );

	if (d.exec() == QDialog::Accepted) {
		setVideoFile( d.videoFile() );
		setDVDDevice( d.DVDDevice() );
		setCols( d.cols() );
		setRows( d.rows() );
		setInitialStep( d.initialStep() );
		setMaxWidth( d.maxWidth() );
		setDisplayOSD( d.displayOSD() );
		setAspectRatio( d.aspectRatio() );
		setExtractFormat(d.format() );
		save_last_directory = d.saveLastDirectory();

		return true;
	}

	return false;
}

void VideoPreview::saveSettings() {
	qDebug("VideoPreview::saveSettings");

	set->beginGroup("videopreview");

	set->setValue("columns", cols());
	set->setValue("rows", rows());
	set->setValue("initial_step", initialStep());
	set->setValue("max_width", maxWidth());
	set->setValue("osd", displayOSD());
	set->setValue("format", extractFormat());
	set->setValue("save_last_directory", save_last_directory);

	if (save_last_directory) {
		set->setValue("last_directory", last_directory);
	}

	set->setValue("filename", videoFile());
	set->setValue("dvd_device", DVDDevice());

	set->setValue("show_info", toggleInfoAct->isChecked());

	set->endGroup();
}

void VideoPreview::loadSettings() {
	qDebug("VideoPreview::loadSettings");

	set->beginGroup("videopreview");

	setCols( set->value("columns", cols()).toInt() );
	setRows( set->value("rows", rows()).toInt() );
	setInitialStep( set->value("initial_step", initialStep()).toInt() );
	setMaxWidth( set->value("max_width", maxWidth()).toInt() );
	setDisplayOSD( set->value("osd", displayOSD()).toBool() );
	setExtractFormat( (ExtractFormat) set->value("format", extractFormat()).toInt() );
	save_last_directory = set->value("save_last_directory", save_last_directory).toBool();
	last_directory = set->value("last_directory", last_directory).toString();

	setVideoFile( set->value("filename", videoFile()).toString() );
	setDVDDevice( set->value("dvd_device", DVDDevice()).toString() );

	toggleInfoAct->setChecked(set->value("show_info", true).toBool());

	set->endGroup();
}

void VideoPreview::adjustWindowSize() {
	qDebug("VideoPreview::adjustWindowSize: window size: %d %d", width(), height());
	qDebug("VideoPreview::adjustWindowSize: scroll_area size: %d %d", scroll_area->width(), scroll_area->height());

	int diff_width = width() - scroll_area->maximumViewportSize().width();
	int diff_height = height() - scroll_area->maximumViewportSize().height();

	qDebug("VideoPreview::adjustWindowSize: diff_width: %d diff_height: %d", diff_width, diff_height);

	QSize new_size = w_contents->size() + QSize( diff_width, diff_height);

	qDebug("VideoPreview::adjustWindowSize: new_size: %d %d", new_size.width(), new_size.height());

	resize(new_size);
}

void VideoPreview::cancelPressed() {
	canceled = true;
}

// Language change stuff
void VideoPreview::changeEvent(QEvent *e) {
	if (e->type() == QEvent::LanguageChange) {
		retranslateStrings();
	} else {
		QWidget::changeEvent(e);
	}
}
