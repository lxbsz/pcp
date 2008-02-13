/*
 * Copyright (c) 2007, Aconex.  All Rights Reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
#include "openviewdialog.h"
#include <QtGui/QCompleter>
#include <QtGui/QFileDialog>
#include <QtGui/QMessageBox>
#include <pcp/pmapi.h>
#include <pcp/impl.h>
#include "main.h"
#include "hostdialog.h"

OpenViewDialog::OpenViewDialog(QWidget *parent) : QDialog(parent)
{
    setupUi(this);
    my.dirModel = new QDirModel;
    my.dirModel->setIconProvider(fileIconProvider);
    dirListView->setModel(my.dirModel);

    my.completer = new QCompleter;
    my.completer->setModel(my.dirModel);
    fileNameLineEdit->setCompleter(my.completer);

    QString home = my.userDir = QDir::homePath();
    my.userDir.append("/.pcp/kmchart");
    my.systemDir = pmGetConfig("PCP_VAR_DIR");
    my.systemDir.append("/config/kmchart");

    pathComboBox->addItem(fileIconProvider->icon(QFileIconProvider::Folder),
			  my.systemDir);
    pathComboBox->addItem(fileIconProvider->icon(QFileIconProvider::Folder),
			  my.userDir);
    pathComboBox->addItem(fileIconProvider->icon(QFileIconProvider::Folder),
			  home);

    proxyComboBox->setEnabled(false);
    proxyPushButton->setEnabled(false);
}

OpenViewDialog::~OpenViewDialog()
{
    delete my.completer;
    delete my.dirModel;
}

void OpenViewDialog::reset()
{
    if ((my.archiveSource = kmchart->isArchiveTab())) {
	sourceLabel->setText(tr("Archive:"));
	sourcePushButton->setIcon(QIcon(":/archive.png"));
	proxyLabel->setText(tr("Host:"));
	proxyPushButton->setEnabled(false);
    } else {
	sourceLabel->setText(tr("Host:"));
	sourcePushButton->setIcon(QIcon(":/computer.png"));
	proxyLabel->setText(tr("Proxy:"));
	proxyComboBox->setEnabled(false);	// WISHLIST: dynamic proxys
	proxyPushButton->setEnabled(false);	// WISHLIST: dynamic proxys
    }
    setupComboBoxes(my.archiveSource);
    setPath(my.systemDir);
}

void OpenViewDialog::setPathUi(const QString &path)
{
    if (path.isEmpty())
	return;

    int index = pathComboBox->findText(path);
    if (index == -1) {
	pathComboBox->addItem(fileIconProvider->icon(QFileIconProvider::Folder),
				path);
	index = pathComboBox->count() - 1;
    }
    pathComboBox->setCurrentIndex(index);
    dirListView->selectionModel()->clear();

    userToolButton->setChecked(path == my.userDir);
    systemToolButton->setChecked(path == my.systemDir);

    fileNameLineEdit->setModified(false);
    fileNameLineEdit->clear();
}

void OpenViewDialog::setPath(const QModelIndex &index)
{
    console->post("OpenViewDialog::setPath QModelIndex path=%s",
			(const char *)my.dirModel->filePath(index).toAscii());
    my.dirIndex = index;
    my.dirModel->refresh(index);
    dirListView->setRootIndex(index);
    setPathUi(my.dirModel->filePath(index));
}

void OpenViewDialog::setPath(const QString &path)
{
    console->post("OpenViewDialog::setPath QString path=%s",
			(const char *)path.toAscii());
    my.dirIndex = my.dirModel->index(path);
    my.dirModel->refresh(my.dirIndex);
    dirListView->setRootIndex(my.dirIndex);
    setPathUi(path);
}

void OpenViewDialog::pathComboBox_currentIndexChanged(QString path)
{
    console->post("OpenViewDialog::pathComboBox_currentIndexChanged");
    setPath(path);
}

void OpenViewDialog::parentToolButton_clicked()
{
    console->post("OpenViewDialog::parentToolButton_clicked");
    setPath(my.dirModel->parent(my.dirIndex));
}

void OpenViewDialog::userToolButton_clicked(bool enabled)
{
    if (enabled) {
	QDir dir;
	if (!dir.exists(my.userDir))
	    dir.mkpath(my.userDir);
	setPath(my.userDir);
    }
}

void OpenViewDialog::systemToolButton_clicked(bool enabled)
{
    if (enabled)
	setPath(my.systemDir);
}

void OpenViewDialog::dirListView_clicked(const QModelIndex &)
{
    QItemSelectionModel *selections = dirListView->selectionModel();
    QModelIndexList selectedIndexes = selections->selectedIndexes();

    console->post("OpenViewDialog::dirListView_clicked");

    my.completer->setCompletionPrefix(my.dirModel->filePath(my.dirIndex));
    if (selectedIndexes.count() != 1)
	fileNameLineEdit->setText("");
    else
	fileNameLineEdit->setText(my.dirModel->fileName(selectedIndexes.at(0)));
}

void OpenViewDialog::dirListView_activated(const QModelIndex &index)
{
    QFileInfo fi = my.dirModel->fileInfo(index);

    console->post("OpenViewDialog::dirListView_activated");

    if (fi.isDir())
	setPath(index);
    else if (fi.isFile()) {
	QStringList files;
	files << fi.absoluteFilePath();
	if (openViewFiles(files) == true)
	    done(0);
    }
}

int OpenViewDialog::setupArchiveComboBoxes()
{
    QIcon archiveIcon = fileIconProvider->icon(FileIconProvider::Archive);
    QIcon hostIcon = fileIconProvider->icon(QFileIconProvider::Computer);
    int index = 0;

    for (unsigned int i = 0; i < archiveGroup->numContexts(); i++) {
	QmcSource source = archiveGroup->context(i)->source();
	sourceComboBox->insertItem(i, archiveIcon, source.source());
	proxyComboBox->insertItem(i, hostIcon, source.host());
	if (i == archiveGroup->contextIndex())
	    index = i;
    }
    return index;
}

int OpenViewDialog::setupLiveComboBoxes()
{
    QIcon hostIcon = fileIconProvider->icon(QFileIconProvider::Computer);
    int index = 0;

    for (unsigned int i = 0; i < liveGroup->numContexts(); i++) {
	QmcSource source = liveGroup->context(i)->source();
	sourceComboBox->insertItem(i, hostIcon, source.host());
	proxyComboBox->insertItem(i, hostIcon, source.proxy());
	if (i == liveGroup->contextIndex())
	    index = i;
    }
    return index;
}

void OpenViewDialog::setupComboBoxes(bool arch)
{
    // We block signals on the target combo boxes so that we don't
    // send spurious signals out about their lists being changed.
    // If we did that, we would keep changing the current context.
    sourceComboBox->blockSignals(true);
    proxyComboBox->blockSignals(true);
    sourceComboBox->clear();
    proxyComboBox->clear();
    int index = arch ? setupArchiveComboBoxes() : setupLiveComboBoxes();
    sourceComboBox->setCurrentIndex(index);
    proxyComboBox->setCurrentIndex(index);
    sourceComboBox->blockSignals(false);
    proxyComboBox->blockSignals(false);
}

void OpenViewDialog::archiveAdd()
{
    QFileDialog *af = new QFileDialog(this);
    QStringList al;
    int sts;

    af->setFileMode(QFileDialog::ExistingFiles);
    af->setAcceptMode(QFileDialog::AcceptOpen);
    af->setWindowTitle(tr("Add Archive"));
    af->setIconProvider(fileIconProvider);
    af->setDirectory(QDir::homePath());

    if (af->exec() == QDialog::Accepted)
	al = af->selectedFiles();
    for (QStringList::Iterator it = al.begin(); it != al.end(); ++it) {
	QString archive = *it;
	if ((sts = archiveGroup->use(PM_CONTEXT_ARCHIVE, archive)) < 0) {
	    archive.prepend(tr("Cannot open PCP archive: "));
	    archive.append(tr("\n"));
	    archive.append(tr(pmErrStr(sts)));
	    QMessageBox::warning(this, pmProgname, archive,
		    QMessageBox::Ok|QMessageBox::Default|QMessageBox::Escape,
		    QMessageBox::NoButton, QMessageBox::NoButton);
	} else {
	    setupComboBoxes(true);
	    archiveGroup->updateBounds();
	    QmcSource source = archiveGroup->context()->source();
	    kmtime->addArchive(source.start(), source.end(),
				source.timezone(), source.host(), false);
	}
    }
    delete af;
}

void OpenViewDialog::hostAdd()
{
    HostDialog *h = new HostDialog(this);
    int sts;

    if (h->exec() == QDialog::Accepted) {
	QString host = h->hostLineEdit->text().trimmed();
	if ((sts = liveGroup->use(PM_CONTEXT_HOST, host)) < 0) {
	    host.prepend(tr("Cannot connect to host: "));
	    host.append(tr("\n"));
	    host.append(tr(pmErrStr(sts)));
	    QMessageBox::warning(this, pmProgname, host,
		    QMessageBox::Ok|QMessageBox::Default|QMessageBox::Escape,
		    QMessageBox::NoButton, QMessageBox::NoButton);
	} else {
	    setupComboBoxes(false);
	}
    }
    delete h;
}

void OpenViewDialog::proxyAdd()
{
}

void OpenViewDialog::proxyPushButton_clicked()
{
    if (my.archiveSource == false)
	proxyAdd();
}

void OpenViewDialog::sourcePushButton_clicked()
{
    if (my.archiveSource)
	archiveAdd();
    else
	hostAdd();
}

void OpenViewDialog::sourceComboBox_currentIndexChanged(int index)
{
    console->post("OpenViewDialog::sourceComboBox_currentIndexChanged %d", index);
    if (my.archiveSource == false)
	liveGroup->use((unsigned int)index);
    else
	archiveGroup->use((unsigned int)index);
}

bool OpenViewDialog::useLiveContext(QString source)
{
    int sts;

    if ((sts = liveGroup->use(PM_CONTEXT_HOST, source)) < 0) {
	QString msg;
	msg.sprintf("Failed to connect to pmcd on \"%s\".\n%s.\n\n",
		    (const char *)source.toAscii(), pmErrStr(sts));
	QMessageBox::warning(NULL, pmProgname, msg,
		QMessageBox::Ok | QMessageBox::Default | QMessageBox::Escape,
		QMessageBox::NoButton, QMessageBox::NoButton);
	return false;
    }
    return true;
}

bool OpenViewDialog::useArchiveContext(QString source)
{
    int sts;

    if ((sts = archiveGroup->use(PM_CONTEXT_ARCHIVE, source)) < 0) {
	QString msg;
	msg.sprintf("Failed to open archive \"%s\".\n%s.\n\n",
		    (const char *)source.toAscii(), pmErrStr(sts));
	QMessageBox::warning(NULL, pmProgname, msg,
		QMessageBox::Ok | QMessageBox::Default | QMessageBox::Escape,
		QMessageBox::NoButton, QMessageBox::NoButton);
	return false;
    }
    return true;
}

bool OpenViewDialog::useComboBoxContext(bool arch)
{
    if (arch == false)
	return useLiveContext(sourceComboBox->currentText());
    else
	return useArchiveContext(sourceComboBox->currentText());
}

bool OpenViewDialog::openViewFiles(const QStringList &fl)
{
    QString msg;
    bool result = true;

    if (kmchart->isArchiveTab() != my.archiveSource) {
	if (kmchart->isArchiveTab())
	    msg = tr("Cannot open Host View(s) in an Archive Tab\n");
	else
	    msg = tr("Cannot open Archive View(s) in a Host Tab\n");
	QMessageBox::warning(this, pmProgname, msg,
	    QMessageBox::Ok|QMessageBox::Default|QMessageBox::Escape,
	    QMessageBox::NoButton, QMessageBox::NoButton);
	return false;
    }
    if (useComboBoxContext(my.archiveSource) == false)
	return false;
    QStringList files = fl;
    for (QStringList::Iterator it = files.begin(); it != files.end(); ++it)
	if (openView((const char *)(*it).toAscii()) == false)
	    result = false;
    kmchart->enableUi();
    return result;
}

void OpenViewDialog::openPushButton_clicked()
{
    QStringList files;
    QString msg;

    if (fileNameLineEdit->isModified()) {
	QString filename = fileNameLineEdit->text().trimmed();
	if (filename.isEmpty())
	    msg = tr("No View file(s) specified");
	else {
	    QFileInfo f(filename);
	    if (f.isDir()) {
		setPath(filename);
	    }
	    else if (f.exists()) {
		files << filename;
		if (openViewFiles(files) == true)
		    done(0);
	    }
	    else {
		msg = tr("No such View file exists:\n");
		msg.append(filename);
	    }
	}
    }
    else {
	QItemSelectionModel *selections = dirListView->selectionModel();
	QModelIndexList selectedIndexes = selections->selectedIndexes();

	if (selectedIndexes.count() == 0)
	    msg = tr("No View file(s) selected");
	else {
	    for (int i = 0; i < selectedIndexes.count(); i++) {
		QString filename = my.dirModel->filePath(selectedIndexes.at(i));
		QFileInfo f(filename);
		if (f.isDir())
		    continue;
		files << filename;
	    }
	    if (files.size() > 0)
		if (openViewFiles(files) == true)
		    done(0);
	}
    }

    if (msg.isEmpty() == false) {
	QMessageBox::warning(this, pmProgname, msg,
	    QMessageBox::Ok|QMessageBox::Default|QMessageBox::Escape,
	    QMessageBox::NoButton, QMessageBox::NoButton);
    }
}
