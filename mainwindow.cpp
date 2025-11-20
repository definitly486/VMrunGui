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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    bhyveProcess = new QProcess(this);
    shouldRestart = false;

    ui->lineEdit->setPlaceholderText("Например: 4G, 8G, 8192M");

    // Подключаем кнопки
    connect(ui->pushButton_start, &QPushButton::clicked, this, &MainWindow::on_pushButton_start_clicked);
    connect(ui->pushButton_stop, &QPushButton::clicked, this, &MainWindow::on_pushButton_stop_clicked);
    connect(ui->pushButton_cleanupTap, &QPushButton::clicked, this, &MainWindow::cleanupAllTapDevices);
    connect(ui->pushButton_clear, &QPushButton::clicked, ui->textEdit, &QTextEdit::clear);

    // ← ЭТО НОВАЯ СТРОКА (и только она!)
    connect(ui->pushButton_arpScan, &QPushButton::clicked, this, &MainWindow::on_pushButton_arpScan_clicked);

    // Вывод от bhyve
    connect(bhyveProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::onVmReadyReadStandardOutput);
    // ... остальное без изменений
    connect(bhyveProcess, &QProcess::readyReadStandardError, this, &MainWindow::onVmReadyReadStandardError);
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

// ======================== Проверка памяти ========================
bool MainWindow::validateMemory(const QString &mem, QString *errorMessage)
{
    if (mem.isEmpty()) {
        if (errorMessage) *errorMessage = "Укажите объём памяти!";
        return false;
    }

    QString input = mem.trimmed();
    QRegularExpression re("^(\\d+)([GMgm])$");          // 4G, 8192M и т.д.
    QRegularExpressionMatch match = re.match(input);

    quint64 value = 0;
    QString unit;

    if (match.hasMatch()) {
        value = match.captured(1).toULongLong();
        unit = match.captured(2).toUpper();
    } else {
        // Поддержка старого формата — просто число (в МБ)
        bool ok;
        value = input.toULongLong(&ok);
        if (!ok || value == 0) {
            if (errorMessage) *errorMessage = "Неверный формат памяти!\n\n"
                                "Корректные примеры:\n"
                                "• 4G\n"
                                "• 8G\n"
                                "• 8192M\n"
                                "• 4096 (будет воспринято как МБ)";
            return false;
        }
        unit = "M"; // считаем, что без буквы — мегабайты
    }

    quint64 valueInMB = (unit == "G") ? value * 1024ULL : value;

    // Ограничения
    if (valueInMB < 256) {
        if (errorMessage) *errorMessage = "Слишком мало памяти!\nМинимально разумно — 256 МБ.";
        return false;
    }
    if (valueInMB > 512 * 1024) { // > 512 ГБ
        if (errorMessage) *errorMessage = QString("Слишком много памяти (%1 ГБ)!\n"
                                    "Максимум разумно — 512 ГБ.").arg(valueInMB / 1024);
        return false;
    }

    return true;
}

// ======================== Кнопки ========================
void MainWindow::on_pushButton_start_clicked()
{
    QString vmName = getVmName().trimmed();

    // Если имя пустое — показываем окно со списком VM
    if (vmName.isEmpty()) {
        QDir vmDir("/ntfs-2TB/vm");
        if (!vmDir.exists()) {
            QMessageBox::warning(this, "Ошибка",
                                 "Папка с виртуальными машинами не найдена:\n/ntfs-2TB/vm\n"
                                 "Убедитесь, что диск примонтирован.");
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
            QMessageBox::information(this, "Нет виртуальных машин",
                                     "Не найдено готовых VM в /ntfs-2TB/vm/\n\n"
                                     "Создайте структуру:\n/ntfs-2TB/vm/имя_машины/имя_машины.img");
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
            QString sizeStr = info.exists()
                                  ? QString::number(info.size() / 1024.0 / 1024 / 1024, 'f', 2) + " ГБ"
                                  : "—";
            QString itemText = QString("%1 (%2, %3)")
                                   .arg(name)
                                   .arg(sizeStr)
                                   .arg(info.lastModified().toString("dd.MM.yyyy hh:mm"));
            auto *item = new QListWidgetItem(itemText);
            item->setData(Qt::UserRole, name);
            listWidget->addItem(item);
        }

        listWidget->setStyleSheet(
            "QListWidget { font-size: 14px; }"
            "QListWidget::item { padding: 12px; }"
            "QListWidget::item:selected { background: #0078d4; color: white; }"
            );

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

    // === ПРОВЕРКА ПАМЯТИ ===
    QString memoryInput = getMemory();
    QString errMsg;
    if (!validateMemory(memoryInput, &errMsg)) {
        QMessageBox::warning(this, "Неверный объём памяти", errMsg);
        return;
    }

    // Нормализуем ввод (приводим к верхнему регистру и добавляем букву, если её нет)
    QString normalized = memoryInput.trimmed();
    if (!normalized.contains(QRegularExpression("[GMgm]$", QRegularExpression::CaseInsensitiveOption))) {
        normalized += "M"; // считаем, что без буквы — мегабайты
    } else {
        normalized = normalized.toUpper();
    }
    ui->lineEdit->setText(normalized); // обновляем поле для красоты

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
        removeTapFromBridge();
        setVmStoppedState();
    });
}

// ======================== Запуск bhyve ========================
void MainWindow::startBhyve()
{
    QString vmName = getVmName();

    // Автоопределение пути к диску
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

    ui->textEdit->append("<b>[Запуск]</b> doas bhyve " + args.join(" "));
    qDebug() << "bhyve args:" << args;

    bhyveProcess->setProgram("doas");
    bhyveProcess->setArguments(QStringList() << "bhyve" << args);
    bhyveProcess->start();

    QTimer::singleShot(800, this, &MainWindow::attachTapToBridge);
}

// ======================== Остальные методы (без изменений) ========================

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
    ui->pushButton_start->setText(running ? "Запускается..." : "Start VM");
}

void MainWindow::setVmStoppedState()
{
    ui->pushButton_start->setEnabled(true);
    ui->pushButton_stop->setEnabled(false);
    ui->pushButton_start->setText("Start VM");
}

void MainWindow::onVmFinished(int exitCode, QProcess::ExitStatus)
{
    ui->textEdit->append("<font color=\"red\"><b>[Завершён]</b> bhyve завершился (код: " + QString::number(exitCode) + ")</font>");
    destroyVm();
    removeTapFromBridge();
    setVmStoppedState();

    if (!shouldRestart || exitCode == 1) {
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
    case QProcess::FailedToStart: msg = "Не удалось запустить (doas/bhyve не найден или нет прав)"; break;
    case QProcess::Crashed: msg = "bhyve аварийно завершился"; break;
    case QProcess::Timedout: msg = "Таймаут процесса"; break;
    default: msg = "Неизвестная ошибка процесса"; break;
    }
    ui->textEdit->append("<font color=\"red\"><b>[ОШИБКА]</b> " + msg + "</font>");
    removeTapFromBridge();
    setVmStoppedState();
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

void MainWindow::on_pushButton_arpScan_clicked()
{
    ui->textEdit->append("<font color=\"magenta\"><b>[ARP-SCAN]</b> Запуск сканирования локальной сети...</font>");

    // Создаём процесс ПЕРЕД connect!
    QProcess* arpProcess = new QProcess(this);

    // Теперь arpProcess уже существует → connect видит его
    connect(arpProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, arpProcess](int, QProcess::ExitStatus) {
                QString output = arpProcess->readAllStandardOutput();
                QString error  = arpProcess->readAllStandardError();

                if (!output.isEmpty()) {
                    ui->textEdit->append("<font color=\"cyan\"><b>[ARP-SCAN Результат]</b></font>");
                    for (const QString& line : output.split('\n', Qt::SkipEmptyParts)) {
                        QString escaped = line.toHtmlEscaped();

                        // IP — зелёный жирный
                        escaped.replace(QRegularExpression(R"(\b\d{1,3}(\.\d{1,3}){3}\b)"),
                                        "<b><font color=\"#00ff00\">\\1</font></b>");

                        // MAC — жёлтый
                        escaped.replace(QRegularExpression(R"([0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5})"),
                                        "<font color=\"#ffff00\">\\1</font>");

                        ui->textEdit->append(escaped);
                    }
                }

                if (!error.isEmpty()) {
                    ui->textEdit->append("<font color=\"red\"><b>[ARP-SCAN Ошибка]</b></font>");
                    ui->textEdit->append("<font color=\"red\">" + error.toHtmlEscaped() + "</font>");
                }

                if (output.isEmpty() && error.isEmpty()) {
                    ui->textEdit->append("<font color=\"gray\">Ничего не найдено (arp-scan не установлен?)</font>");
                }

                ui->textEdit->append("<font color=\"magenta\"><b>[ARP-SCAN]</b> Сканирование завершено.</font>");
                ui->pushButton_arpScan->setEnabled(true);

                arpProcess->deleteLater();
            });

    // Запускаем через doas
    arpProcess->setProgram("doas");
    arpProcess->setArguments({"arp-scan", "--localnet"});
    arpProcess->start();
}


// ======================== Геттеры ========================
QString MainWindow::getVmName() const { return ui->lineEdit_2->text().trimmed(); }
QString MainWindow::getMemory() const { return ui->lineEdit->text().trimmed(); }
QString MainWindow::getDiskPath() const { return ui->lineEdit_3->text().trimmed(); }
QString MainWindow::getIsoPath() const { return ui->lineEdit_4->text().trimmed(); }
QString MainWindow::getTapInterface() const { return ui->lineEdit_5->text().trimmed(); }
