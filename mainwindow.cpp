#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QMessageBox>
#include <QTimer>
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QDialog>
#include <QVBoxLayout>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QRegularExpression>
#include <QPushButton>
#include <QHBoxLayout>
#include <QTextEdit>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_arpDialog(nullptr)
    , m_arpTextEdit(nullptr)
    , bhyveProcess(new QProcess(this))
    , shouldRestart(false)
{
    ui->setupUi(this);
    ui->lineEdit->setPlaceholderText("Например: 4G, 8G, 8192M");

    connect(ui->pushButton_start, &QPushButton::clicked, this, &MainWindow::on_pushButton_start_clicked);
    connect(ui->pushButton_stop, &QPushButton::clicked, this, &MainWindow::on_pushButton_stop_clicked);
    connect(ui->pushButton_cleanupTap, &QPushButton::clicked, this, &MainWindow::cleanupAllTapDevices);
    connect(ui->pushButton_clear, &QPushButton::clicked, ui->textEdit, &QTextEdit::clear);
    connect(ui->pushButton_arpScan, &QPushButton::clicked, this, &MainWindow::on_pushButton_arpScan_clicked);

    // === Самое важное: кнопка Stop активна ТОЛЬКО когда процесс реально запущен ===
    connect(bhyveProcess, &QProcess::started, this, [this]() {
        setVmRunningState(true);
        ui->textEdit->append("<font color=\"#50fa7b\"><b>[ЗАПУЩЕНО]</b> Виртуальная машина успешно стартовала</font>");
    });

    connect(bhyveProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::onVmReadyReadStandardOutput);
    connect(bhyveProcess, &QProcess::readyReadStandardError, this, &MainWindow::onVmReadyReadStandardError);
    connect(bhyveProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::onVmFinished);
    connect(bhyveProcess, &QProcess::errorOccurred, this, &MainWindow::onVmErrorOccurred);

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

// ======================== ARP-SCAN ========================
void MainWindow::on_pushButton_arpScan_clicked()
{
    ui->pushButton_arpScan->setEnabled(false);
    showArpScanDialog();
}

void MainWindow::showArpScanDialog()
{
    if (!m_arpDialog) {
        m_arpDialog = new QDialog(this);
        m_arpDialog->setWindowTitle("Сканирование сети — arp-scan --localnet");
        m_arpDialog->resize(800, 600);
        m_arpDialog->setModal(false);

        auto *layout = new QVBoxLayout(m_arpDialog);
        m_arpTextEdit = new QTextEdit(m_arpDialog);
        m_arpTextEdit->setReadOnly(true);
        m_arpTextEdit->setFontFamily("Monospace");
        m_arpTextEdit->setStyleSheet("QTextEdit { background-color: #1e1e1e; color: #d4d4d4; font-size: 13px; }");
        layout->addWidget(m_arpTextEdit);

        auto *btnLayout = new QHBoxLayout();
        auto *copyBtn = new QPushButton("Копировать всё", m_arpDialog);
        auto *closeBtn = new QPushButton("Закрыть", m_arpDialog);
        btnLayout->addWidget(copyBtn);
        btnLayout->addStretch();
        btnLayout->addWidget(closeBtn);
        layout->addLayout(btnLayout);

        connect(copyBtn, &QPushButton::clicked, this, [this]() {
            m_arpTextEdit->selectAll();
            m_arpTextEdit->copy();
            QMessageBox::information(m_arpDialog, "Готово", "Результат сканирования скопирован в буфер обмена");
        });
        connect(closeBtn, &QPushButton::clicked, m_arpDialog, &QDialog::close);
    }

    m_arpTextEdit->clear();
    m_arpTextEdit->append("<b style=\"color:#ff79c6\">[ARP-SCAN]</b> Запуск сканирования локальной сети...<br>");

    QProcess *arpProcess = new QProcess(this);
    connect(arpProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, arpProcess]() {
                QString output = arpProcess->readAllStandardOutput();
                QString error  = arpProcess->readAllStandardError();

                if (!output.isEmpty()) {
                    m_arpTextEdit->append("<b style=\"color:#8be9fd\">[Результат]</b><br>");
                    for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
                        QString escaped = line.toHtmlEscaped();
                        escaped.replace(QRegularExpression(R"(\b\d{1,3}(\.\d{1,3}){3}\b)"),
                                        "<b style=\"color:#50fa7b\">\\1</b>");
                        escaped.replace(QRegularExpression(R"([0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5})"),
                                        "<span style=\"color:#f1fa8c\">\\1</span>");
                        escaped.replace(QRegularExpression(R"(\t\t.*)"),
                                        "<span style=\"color:#bd93f9\">\\1</span>");
                        m_arpTextEdit->append(escaped + "<br>");
                    }
                }
                if (!error.isEmpty()) {
                    m_arpTextEdit->append("<b style=\"color:#ff5555\">[ОШИБКА]</b><br>");
                    m_arpTextEdit->append("<span style=\"color:#ff6e6e\">" + error.toHtmlEscaped().replace("\n", "<br>") + "</span>");
                }
                if (output.isEmpty() && error.isEmpty()) {
                    m_arpTextEdit->append("<span style=\"color:#888\">Ничего не найдено (arp-scan не установлен или нет прав)</span>");
                }

                m_arpTextEdit->append("<br><b style=\"color:#ff79c6\">[ARP-SCAN]</b> Сканирование завершено.");
                ui->pushButton_arpScan->setEnabled(true);
                arpProcess->deleteLater();
            });

    arpProcess->setProgram("doas");
    arpProcess->setArguments({"arp-scan", "--localnet"});
    arpProcess->start();
    m_arpDialog->show();
    m_arpDialog->raise();
    m_arpDialog->activateWindow();
}

// ======================== Валидация памяти ========================
bool MainWindow::validateMemory(const QString &mem, QString *errorMessage)
{
    if (mem.isEmpty()) {
        if (errorMessage) *errorMessage = "Укажите объём памяти!";
        return false;
    }
    QString input = mem.trimmed();
    QRegularExpression re("^(\\d+)([GMgm])$");
    QRegularExpressionMatch match = re.match(input);
    quint64 value = 0;
    QString unit;

    if (match.hasMatch()) {
        value = match.captured(1).toULongLong();
        unit = match.captured(2).toUpper();
    } else {
        bool ok;
        value = input.toULongLong(&ok);
        if (!ok || value == 0) {
            if (errorMessage) *errorMessage = "Неверный формат памяти!\n\nКорректные примеры:\n• 4G\n• 8G\n• 8192M\n• 4096 (МБ)";
            return false;
        }
        unit = "M";
    }

    quint64 valueInMB = (unit == "G") ? value * 1024ULL : value;
    if (valueInMB < 256) {
        if (errorMessage) *errorMessage = "Слишком мало памяти! Минимум 256 МБ.";
        return false;
    }
    if (valueInMB > 512 * 1024) {
        if (errorMessage) *errorMessage = QString("Слишком много памяти (%1 ГБ)! Максимум 512 ГБ.").arg(valueInMB / 1024);
        return false;
    }
    return true;
}

// ======================== Start VM ========================
void MainWindow::on_pushButton_start_clicked()
{
    if (bhyveProcess->state() != QProcess::NotRunning) {
        QMessageBox::information(this, "Уже запущено", "ВМ уже запущена или в процессе запуска.");
        return;
    }

    QString vmName = getVmName().trimmed();
    if (vmName.isEmpty()) {
        // Диалог выбора VM (полностью ваш оригинальный код)
        QDir vmDir("/ntfs-2TB/vm");
        if (!vmDir.exists()) {
            QMessageBox::warning(this, "Ошибка", "Папка с виртуальными машинами не найдена:\n/ntfs-2TB/vm\nУбедитесь, что диск примонтирован.");
            return;
        }
        QStringList existingVMs;
        const QStringList dirs = vmDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &dir : dirs) {
            QString imgPath = vmDir.absoluteFilePath(dir + "/" + dir + ".img");
            if (QFileInfo::exists(imgPath)) {
                existingVMs << dir;
            }
        }
        if (existingVMs.isEmpty()) {
            QMessageBox::information(this, "Нет виртуальных машин", "Не найдено готовых VM в /ntfs-2TB/vm/\n\nСоздайте структуру:\n/ntfs-2TB/vm/имя_машины/имя_машины.img");
            return;
        }

        QDialog dialog(this);
        dialog.setWindowTitle("Выберите виртуальную машину");
        dialog.setMinimumWidth(420);
        dialog.setMinimumHeight(560);
        auto *listWidget = new QListWidget(&dialog);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(listWidget);

        for (const QString &name : existingVMs) {
            QString imgPath = vmDir.absoluteFilePath(name + "/" + name + ".img");
            QFileInfo info(imgPath);
            QString sizeStr = info.exists() ? QString::number(info.size() / 1024.0 / 1024 / 1024, 'f', 2) + " ГБ" : "—";
            QString itemText = QString("%1 (%2, %3)").arg(name).arg(sizeStr).arg(info.lastModified().toString("dd.MM.yyyy hh:mm"));
            auto *item = new QListWidgetItem(itemText);
            item->setData(Qt::UserRole, name);
            listWidget->addItem(item);
        }

        listWidget->setStyleSheet("QListWidget { font-size: 14px; } QListWidget::item { padding: 12px; } QListWidget::item:selected { background: #0078d4; color: white; }");
        connect(listWidget, &QListWidget::itemDoubleClicked, &dialog, [&](QListWidgetItem *item) {
            ui->lineEdit_2->setText(item->data(Qt::UserRole).toString());
            dialog.accept();
            if (!getMemory().isEmpty()) {
                QTimer::singleShot(100, this, &MainWindow::on_pushButton_start_clicked);
            }
        });

        auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttonBox);
        connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        if (dialog.exec() == QDialog::Accepted && listWidget->currentItem()) {
            ui->lineEdit_2->setText(listWidget->currentItem()->data(Qt::UserRole).toString());
        }
        return;
    }

    QString memoryInput = getMemory();
    QString errMsg;
    if (!validateMemory(memoryInput, &errMsg)) {
        QMessageBox::warning(this, "Неверный объём памяти", errMsg);
        return;
    }

    QString normalized = memoryInput.trimmed();
    if (!normalized.contains(QRegularExpression("[GMgm]$", QRegularExpression::CaseInsensitiveOption))) {
        normalized += "M";
    } else {
        normalized = normalized.toUpper();
    }
    ui->lineEdit->setText(normalized);  // ← ИСПРАВЛЕНО: было "normally"

    shouldRestart = true;
    ui->textEdit->append("<font color=\"#ff79c6\"><b>[Старт]</b> Подготовка к запуску VM: " + vmName + "</font>");
    startBhyve();
}

// ======================== Запуск bhyve ========================
void MainWindow::startBhyve()
{
    QString vmName = getVmName();
    QString specialDiskPath = QString("/ntfs-2TB/vm/%1/%1.img").arg(vmName);
    QString diskPath = QFileInfo::exists(specialDiskPath) ? specialDiskPath : getDiskPath();

    if (!QFileInfo::exists(diskPath)) {
        ui->textEdit->append("<font color=\"red\"><b>[Ошибка]</b> Диск не найден: " + diskPath.toHtmlEscaped() + "</font>");
        setVmStoppedState();
        return;
    }

    QString diskDevice = diskPath.startsWith("/ntfs-2TB/vm/") ? "virtio-blk" : "ahci-hd";

    QStringList args = {
        "-c", "1",
        "-s", "0,hostbridge",
        "-s", QString("3,%1,%2").arg(diskDevice, diskPath),
    };

    if (!getIsoPath().isEmpty() && QFileInfo::exists(getIsoPath())) {
        args << "-s" << QString("4,ahci-cd,%1").arg(getIsoPath());
    }

    QString tapInterface = getTapInterface().trimmed();
    if (tapInterface.isEmpty()) {
        tapInterface = "tap0";
        ui->lineEdit_5->setText(tapInterface);
    }

    args << "-s" << QString("10,virtio-net,%1").arg(tapInterface);
    args << "-s" << "15,virtio-9p,sharename=/home/";
    args << "-s" << "30,fbuf,tcp=0.0.0.0:5900,w=1920,h=1080";
    args << "-s" << "31,lpc";
    args << "-l" << "bootrom,/usr/local/share/uefi-firmware/BHYVE_UEFI.fd";
    args << "-m" << getMemory();
    args << "-H" << "-w" << "-P" << "-S";
    args << vmName;

    ui->textEdit->append("<b>[Команда]</b> doas bhyve " + args.join(" "));

    bhyveProcess->setProgram("doas");
    bhyveProcess->setArguments(QStringList() << "bhyve" << args);

    // Состояние "Running" ставим ТОЛЬКО в сигнале started()
    ui->pushButton_start->setText("Запускается...");
    bhyveProcess->start();  // ← если не запустится — попадём в errorOccurred
}

// ======================== Остановка и состояние кнопок ========================
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
        removeTapFromBridge();
        setVmStoppedState();
    });
}

void MainWindow::attachTapToBridge()
{
    QString tap = getTapInterface().trimmed();
    if (tap.isEmpty()) {
        ui->textEdit->append("<font color=\"orange\"><b>[Bridge]</b> tap не указан — пропуск добавления в bridge0</font>");
        return;
    }

    QProcess check;
    check.start("ifconfig", QStringList() << tap);
    check.waitForFinished(3000);
    if (check.exitCode() != 0) {
        QTimer::singleShot(500, this, &MainWindow::attachTapToBridge);
        return;
    }

    ui->textEdit->append("<font color=\"blue\"><b>[Bridge]</b> Добавляем " + tap + " в bridge0...</font>");
    QProcess *p = new QProcess(this);
    p->setProgram("doas");
    p->setArguments(QStringList() << "ifconfig" << "bridge0" << "addm" << tap << "up");
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [=]() {
        QString out = p->readAllStandardOutput().trimmed();
        QString err = p->readAllStandardError().trimmed();
        if (p->exitCode() == 0) {
            ui->textEdit->append("<font color=\"green\"><b>[Bridge]</b> " + tap + " успешно добавлен в bridge0</font>");
        } else {
            ui->textEdit->append("<font color=\"red\"><b>[Bridge]</b> Ошибка добавления в bridge0</font>");
            if (!err.isEmpty()) ui->textEdit->append("<font color=\"red\">" + err.toHtmlEscaped() + "</font>");
        }
        p->deleteLater();
    });
    p->start();
}

void MainWindow::removeTapFromBridge()
{
    QString tap = getTapInterface().trimmed();
    if (tap.isEmpty()) return;
    QProcess::execute("doas", QStringList() << "ifconfig" << "bridge0" << "deletem" << tap);
    ui->textEdit->append("<font color=\"blue\"><b>[Bridge]</b> " + tap + " удалён из bridge0</font>");
}

void MainWindow::setVmRunningState(bool running)
{
    ui->pushButton_start->setEnabled(!running);
    ui->pushButton_stop->setEnabled(running);
    ui->pushButton_start->setText(running ? "Запущено" : "Start VM");
}

void MainWindow::setVmStoppedState()
{
    ui->pushButton_start->setEnabled(true);
    ui->pushButton_stop->setEnabled(false);
    ui->pushButton_start->setText("Start VM");
}

// ======================== Обработчики завершения ========================
void MainWindow::onVmFinished(int exitCode, QProcess::ExitStatus)
{
    ui->textEdit->append("<font color=\"red\"><b>[Завершён]</b> bhyve завершился (код: " + QString::number(exitCode) + ")</font>");
    destroyVm();
    removeTapFromBridge();
    setVmStoppedState();

    if (!shouldRestart || exitCode != 0) {
        ui->textEdit->append("<b>Авторестарт отключён.</b>");
        return;
    }

    ui->textEdit->append("<font color=\"orange\">Авторестарт через 5 секунд...</font>");
    ui->pushButton_start->setText("Перезапустится...");
    QTimer::singleShot(5000, this, &MainWindow::startBhyve);
}

void MainWindow::onVmErrorOccurred(QProcess::ProcessError error)
{
    QString msg;
    switch (error) {
    case QProcess::FailedToStart:  msg = "Не удалось запустить (doas/bhyve не найден или нет прав)"; break;
    case QProcess::Crashed:        msg = "bhyve аварийно завершился"; break;
    case QProcess::Timedout:       msg = "Таймаут процесса"; break;
    default:                       msg = "Неизвестная ошибка процесса"; break;
    }
    ui->textEdit->append("<font color=\"red\"><b>[ОШИБКА]</b> " + msg + "</font>");
    setVmStoppedState();
    shouldRestart = false;
}

void MainWindow::destroyVm()
{
    QProcess destroyer;
    destroyer.start("doas", QStringList() << "bhyvectl" << "--destroy" << "--vm=" + getVmName());
    destroyer.waitForFinished(8000);
}

void MainWindow::onVmReadyReadStandardOutput()
{
    QString data = bhyveProcess->readAllStandardOutput();
    for (QString line : data.split('\n', Qt::SkipEmptyParts))
        ui->textEdit->append("<font color=\"green\">" + line.toHtmlEscaped() + "</font>");
}

void MainWindow::onVmReadyReadStandardError()
{
    QString data = bhyveProcess->readAllStandardError();
    for (QString line : data.split('\n', Qt::SkipEmptyParts))
        ui->textEdit->append("<font color=\"red\">[ERR] " + line.toHtmlEscaped() + "</font>");
}

void MainWindow::cleanupAllTapDevices()
{
    ui->textEdit->append("<b>[Очистка]</b> Уничтожаем все tap-интерфейсы...");
    QProcess p;
    p.start("doas", QStringList() << "sh" << "-c"
                                  << "for t in /dev/tap*; do [ -e \"$t\" ] && ifconfig ${t#/dev/} destroy && echo \"Уничтожен ${t#/dev/}\"; done || echo \"Нет tap-устройств\"");
    p.waitForFinished(8000);
    QString out = p.readAllStandardOutput().trimmed();
    QString err = p.readAllStandardError().trimmed();
    if (!out.isEmpty()) ui->textEdit->append(out);
    if (!err.isEmpty()) ui->textEdit->append("<font color=\"red\">" + err.toHtmlEscaped() + "</font>");
    if (out.isEmpty() && err.isEmpty()) ui->textEdit->append("Нет занятых tap-устройств");
    ui->textEdit->append("<b>[Готово]</b> Очистка завершена");
}

// ======================== Геттеры ========================
QString MainWindow::getVmName() const       { return ui->lineEdit_2->text().trimmed(); }
QString MainWindow::getMemory() const       { return ui->lineEdit->text().trimmed(); }
QString MainWindow::getDiskPath() const     { return ui->lineEdit_3->text().trimmed(); }
QString MainWindow::getIsoPath() const      { return ui->lineEdit_4->text().trimmed(); }
QString MainWindow::getTapInterface() const { return ui->lineEdit_5->text().trimmed(); }
