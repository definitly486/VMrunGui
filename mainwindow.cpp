#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QMessageBox>
#include <QTimer>
#include <QDateTime>
#include <QDebug>                  // ← обязательно на FreeBSD!
#include <QFileInfo>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    bhyveProcess = new QProcess(this);

    connect(ui->pushButton_start, &QPushButton::clicked, this, &MainWindow::on_pushButton_start_clicked);
    connect(ui->pushButton_stop,  &QPushButton::clicked, this, &MainWindow::on_pushButton_stop_clicked);

    connect(bhyveProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::onVmReadyReadStandardOutput);
    connect(bhyveProcess, &QProcess::readyReadStandardError,  this, &MainWindow::onVmReadyReadStandardError);
    connect(bhyveProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::onVmFinished);
    connect(bhyveProcess, &QProcess::errorOccurred, this, &MainWindow::onVmErrorOccurred);

    connect(ui->pushButton_clear, &QPushButton::clicked, ui->textEdit, &QTextEdit::clear);

    ui->textEdit->setReadOnly(true);
}

MainWindow::~MainWindow()
{
    if (bhyveProcess && bhyveProcess->state() == QProcess::Running) {
        shouldRestart = false;
        bhyveProcess->kill();
        destroyVm();
    }
    delete ui;
}

void MainWindow::on_pushButton_start_clicked()
{
    if (getMemory().isEmpty() || getVmName().isEmpty() || getDiskPath().isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Заполните имя VM, память и путь к диску!");
        return;
    }

    shouldRestart = true;
    startBhyve();

    ui->pushButton_start->setEnabled(false);
    ui->pushButton_stop->setEnabled(true);
    ui->pushButton_start->setText("Запускается...");
}

void MainWindow::on_pushButton_stop_clicked()
{
    shouldRestart = false;

    ui->pushButton_stop->setEnabled(false);
    ui->pushButton_start->setText("Останавливается...");
    ui->textEdit->append("<font color=\"orange\"><b>[Остановка]</b> Принудительная остановка VM...</font>");

    // Сначала мягко
    bhyveProcess->terminate();

    // Через 4 секунды — принудительно убиваем и сразу уничтожаем VM
    QTimer::singleShot(4000, this, [this]() {
        if (bhyveProcess->state() == QProcess::Running) {
            ui->textEdit->append("<font color=\"red\"><b>[KILL]</b> bhyve не отреагировал на terminate → kill</font>");
            bhyveProcess->kill();
        }

        // Сразу уничтожаем VM через bhyvectl (это важно, иначе остаётся висеть в списке)
        destroyVm();

        // Возвращаем кнопки в исходное состояние
        ui->pushButton_start->setEnabled(true);
        ui->pushButton_stop->setEnabled(false);
        ui->pushButton_start->setText("Start VM");
    });
}

void MainWindow::startBhyve()
{
    QString vmName = getVmName();
    QString diskPath = getDiskPath();  // например: /vm/windows.img

    QStringList args = {
        "-c", "1",
        "-s", "0,hostbridge",
        // Меняем на virtio-blk — идеально для .img и raw-файлов
        "-s", QString("3,ahci-hd,%1").arg(diskPath),
        // bootindex=1 больше не нужен для virtio-blk, но можно оставить — не мешает
    };

    // Если указан ISO — подключаем как ahci-cd (это нормально)
    if (!getIsoPath().isEmpty() && QFileInfo::exists(getIsoPath())) {
        args << "-s" << QString("4,ahci-cd,%1").arg(getIsoPath());
    }

    // Остальные параметры без изменений
    args << "-s" << QString("10,virtio-net,%1").arg(getTapInterface());
    args << "-s" << "15,virtio-9p,sharename=/home/";
    args << "-s" << "30,fbuf,tcp=0.0.0.0:5900,w=1920,h=1080,wait";  // VNC
    args << "-s" << "31,lpc";
    args << "-l" << "bootrom,/usr/local/share/uefi-firmware/BHYVE_UEFI.fd";
    args << "-m" << getMemory();
    args << "-H" << "-w" << "-P" << "-S";
    args << vmName;

    ui->textEdit->append("<b>[Запуск]</b> doas bhyve " + args.join(" "));
    qDebug() << "bhyve args:" << args;

    bhyveProcess->setProgram("doas");
    bhyveProcess->setArguments(QStringList() << "bhyve" << args);
    bhyveProcess->start();
}

void MainWindow::onVmFinished(int exitCode, QProcess::ExitStatus)
{
    ui->textEdit->append("<font color=\"red\"><b>[Завершен]</b> bhyve завершился (код: " + QString::number(exitCode) + ")</font>");

    destroyVm();  // на всякий случай ещё раз

    // Возвращаем кнопки в исходное состояние
    ui->pushButton_start->setEnabled(true);
    ui->pushButton_stop->setEnabled(false);
    ui->pushButton_start->setText("Start VM");

    if (exitCode == 1 || !shouldRestart) {
        ui->textEdit->append("<b>Авторестарт отключён.</b>");
        return;
    }

    ui->textEdit->append("Авторестарт через 5 секунд...");
    QTimer::singleShot(5000, this, &MainWindow::startBhyve);
    ui->pushButton_start->setText("Перезапустится...");
}

void MainWindow::destroyVm()
{
    QProcess bhyvectl;
    bhyvectl.start("doas", QStringList() << "bhyvectl" << "--destroy" << "--vm=" + getVmName());
    bhyvectl.waitForFinished(10000);
}

void MainWindow::onVmReadyReadStandardOutput()
{
    QString data = bhyveProcess->readAllStandardOutput();
    for (QString line : data.split('\n'))
        if (!line.isEmpty())
            ui->textEdit->append("<font color=\"green\">" + line.toHtmlEscaped() + "</font>");
}

void MainWindow::onVmReadyReadStandardError()
{
    QString data = bhyveProcess->readAllStandardError();
    for (QString line : data.split('\n'))
        if (!line.isEmpty())
            ui->textEdit->append("<font color=\"red\">[ERR] " + line.toHtmlEscaped() + "</font>");
}

void MainWindow::onVmErrorOccurred(QProcess::ProcessError error)
{
    ui->textEdit->append("<font color=\"darkred\"><b>[ОШИБКА]</b> " + bhyveProcess->errorString() + "</font>");
}

// Геттеры
QString MainWindow::getVmName() const       { return ui->lineEdit_2->text().trimmed(); }
QString MainWindow::getMemory() const       { return ui->lineEdit->text().trimmed(); }
QString MainWindow::getDiskPath() const     { return ui->lineEdit_3->text().trimmed(); }
QString MainWindow::getIsoPath() const      { return ui->lineEdit_4->text().trimmed(); }
QString MainWindow::getTapInterface() const{ return ui->lineEdit_5->text().trimmed(); }
