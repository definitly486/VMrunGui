#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QMessageBox>
#include <QTimer>
#include <QDebug>
#include <QFileInfo>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    bhyveProcess = new QProcess(this);
    shouldRestart = false;

    // Подключаем кнопки
    connect(ui->pushButton_start,       &QPushButton::clicked, this, &MainWindow::on_pushButton_start_clicked);
    connect(ui->pushButton_stop,        &QPushButton::clicked, this, &MainWindow::on_pushButton_stop_clicked);
    connect(ui->pushButton_cleanupTap,  &QPushButton::clicked, this, &MainWindow::cleanupAllTapDevices);
    connect(ui->pushButton_clear,       &QPushButton::clicked, ui->textEdit, &QTextEdit::clear);

    // Вывод от bhyve
    connect(bhyveProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::onVmReadyReadStandardOutput);
    connect(bhyveProcess, &QProcess::readyReadStandardError,  this, &MainWindow::onVmReadyReadStandardError);
    connect(bhyveProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::onVmFinished);
    connect(bhyveProcess, &QProcess::errorOccurred, this, &MainWindow::onVmErrorOccurred);

    // Начальное состояние кнопок
    setVmStoppedState();
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

// ======================== Кнопки ========================

void MainWindow::on_pushButton_start_clicked()
{
    if (getVmName().isEmpty() || getMemory().isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Заполните имя VM и объём памяти!");
        return;
    }

    shouldRestart = true;
    setVmRunningState(true);
    startBhyve();
}

void MainWindow::on_pushButton_stop_clicked()
{
    shouldRestart = false;
    ui->textEdit->append("<font color=\"orange\"><b>[Остановка]</b> Остановка виртуальной машины...</font>");

    bhyveProcess->terminate();
    QTimer::singleShot(4000, this, [this]() {
        if (bhyveProcess->state() == QProcess::Running) {
            ui->textEdit->append("<font color=\"red\"><b>[KILL]</b> Принудительный kill процесса</font>");
            bhyveProcess->kill();
        }
        destroyVm();
        setVmStoppedState();
    });
}

// ======================== Запуск bhyve ========================

void MainWindow::startBhyve()
{
    QString vmName = getVmName();

    // Автоопределение пути к диску: /ntfs-2TB/vm/alpine/alpine.img и т.д.
    QString specialDiskPath = QString("/ntfs-2TB/vm/%1/%1.img").arg(vmName);
    QString diskPath;

    if (QFileInfo::exists(specialDiskPath)) {
        diskPath = specialDiskPath;
        qDebug() << "Используется специальный диск:" << diskPath;
    } else {
        diskPath = getDiskPath();
        qDebug() << "Используется стандартный диск:" << diskPath;
    }

    if (!QFileInfo::exists(diskPath)) {
        ui->textEdit->append("<font color=\"red\"><b>[Ошибка]</b> Диск не найден: " + diskPath + "</font>");
        qWarning() << "Disk image not found:" << diskPath;
        setVmStoppedState();
        return;
    }

    // virtio-blk для Linux-образов на ntfs-2TB, ahci-hd для остальных (Windows)
    QString diskDevice = diskPath.startsWith("/ntfs-2TB/vm/") ? "virtio-blk" : "ahci-hd";

    QStringList args = {
        "-c", "1",
        "-s", "0,hostbridge",
        "-s", QString("3,%1,%2").arg(diskDevice, diskPath),
    };

    if (!getIsoPath().isEmpty() && QFileInfo::exists(getIsoPath())) {
        args << "-s" << QString("4,ahci-cd,%1").arg(getIsoPath());
    }

    args << "-s" << QString("10,virtio-net,%1").arg(getTapInterface());
    args << "-s" << "15,virtio-9p,sharename=/home/";
    args << "-s" << "30,fbuf,tcp=0.0.0.0:5900,w=1920,h=1080,wait";
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

// ======================== Состояния кнопок ========================

void MainWindow::setVmRunningState(bool running)
{
    ui->pushButton_start->setEnabled(!running);
    ui->pushButton_stop->setEnabled(running);
    ui->pushButton_start->setText(running ? "Запускается..." : "Start VM");
}

void MainWindow::setVmStoppedState()
{
    ui->pushButton_start->setEnabled(true);
    ui->pushButton_stop->setEnabled(false);
    ui->pushButton_start->setText("Start VM");
}

// ======================== Обработка завершения и ошибок ========================

void MainWindow::onVmFinished(int exitCode, QProcess::ExitStatus)
{
    ui->textEdit->append("<font color=\"red\"><b>[Завершен]</b> bhyve завершился (код: " + QString::number(exitCode) + ")</font>");
    destroyVm();
    setVmStoppedState();

    if (!shouldRestart || exitCode == 1) {
        ui->textEdit->append("<b>Авторестарт отключён.</b>");
        return;
    }

    ui->textEdit->append("Авторестарт через 5 секунд...");
    ui->pushButton_start->setText("Перезапустится...");
    QTimer::singleShot(5000, this, &MainWindow::startBhyve);
}

void MainWindow::onVmErrorOccurred(QProcess::ProcessError error)
{
    QString msg;
    switch (error) {
    case QProcess::FailedToStart:  msg = "Не удалось запустить (doas/bhyve не найден, нет прав или tap занят)"; break;
    case QProcess::Crashed:        msg = "bhyve аварийно завершился"; break;
    case QProcess::Timedout:       msg = "Таймаут процесса"; break;
    case QProcess::ReadError:      msg = "Ошибка чтения из процесса"; break;
    case QProcess::WriteError:     msg = "Ошибка записи в процесс"; break;
    default:                       msg = "Неизвестная ошибка процесса"; break;
    }

    ui->textEdit->append("<font color=\"red\"><b>[ОШИБКА]</b> " + msg + "</font>");
    qWarning() << "Bhyve process error:" << error;

    setVmStoppedState();
}

void MainWindow::destroyVm()
{
    QProcess destroyer;
    destroyer.start("doas", QStringList() << "bhyvectl" << "--destroy" << "--vm=" + getVmName());
    destroyer.waitForFinished(8000);
}

// ======================== Логи ========================

void MainWindow::onVmReadyReadStandardOutput()
{
    QString data = bhyveProcess->readAllStandardOutput();
    for (const QString &line : data.split('\n', Qt::SkipEmptyParts))
        ui->textEdit->append("<font color=\"green\">" + line.toHtmlEscaped() + "</font>");
}

void MainWindow::onVmReadyReadStandardError()
{
    QString data = bhyveProcess->readAllStandardError();
    for (const QString &line : data.split('\n', Qt::SkipEmptyParts))
        ui->textEdit->append("<font color=\"red\">[ERR] " + line.toHtmlEscaped() + "</font>");
}

// ======================== Очистка tap ========================

void MainWindow::cleanupAllTapDevices()
{
    ui->textEdit->append("<b>[Очистка]</b> Уничтожаем все tap-интерфейсы...");

    QProcess p;
    p.start("doas", QStringList() << "sh" << "-c"
                                  << "for t in /dev/tap*; do "
                                     "[ -e \"$t\" ] && ifconfig ${t#/dev/} destroy && echo \"Уничтожен ${t#/dev/}\"; "
                                     "done || echo \"Нет занятых tap-устройств\"");
    p.waitForFinished(5000);

    QString out = p.readAllStandardOutput().trimmed();
    QString err = p.readAllStandardError().trimmed();

    if (!out.isEmpty()) ui->textEdit->append(out);
    if (!err.isEmpty()) ui->textEdit->append("<font color=\"red\">" + err + "</font>");
    if (out.isEmpty() && err.isEmpty()) ui->textEdit->append("Нет занятых tap-устройств");

    ui->textEdit->append("<b>[Готово]</b> Очистка tap завершена");
}

// ======================== Геттеры ========================

QString MainWindow::getVmName() const        { return ui->lineEdit_2->text().trimmed(); }
QString MainWindow::getMemory() const        { return ui->lineEdit->text().trimmed(); }
QString MainWindow::getDiskPath() const      { return ui->lineEdit_3->text().trimmed(); }
QString MainWindow::getIsoPath() const       { return ui->lineEdit_4->text().trimmed(); }
QString MainWindow::getTapInterface() const  { return ui->lineEdit_5->text().trimmed(); }
