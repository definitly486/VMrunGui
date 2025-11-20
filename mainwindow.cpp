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
#include <QListWidget>
#include <QClipboard>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)                // ← ui должен быть ПЕРВЫМ!
    , m_arpDialog(nullptr)
    , m_arpTextEdit(nullptr)
    , m_runningVmsDialog(nullptr)            // ← добавь, если ещё нет
    , m_runningVmsTextEdit(nullptr)          // ← добавь
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
    connect(ui->pushButton_closeWithoutKilling, &QPushButton::clicked,
            this, &MainWindow::on_pushButton_closeWithoutKilling_clicked);
    connect(bhyveProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::onVmReadyReadStandardOutput);
    connect(bhyveProcess, &QProcess::readyReadStandardError, this, &MainWindow::onVmReadyReadStandardError);
    connect(bhyveProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::onVmFinished);
    connect(bhyveProcess, &QProcess::errorOccurred, this, &MainWindow::onVmErrorOccurred);

    setVmStoppedState();
}

MainWindow::~MainWindow()
{

    delete ui;
}

// ======================== ARP-SCAN в отдельном окне ========================
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
        m_arpTextEdit->setStyleSheet(
            "QTextEdit { background-color: #1e1e1e; color: #d4d4d4; font-size: 13px; }"
            );
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

                        // IP — зелёный жирный
                        escaped.replace(QRegularExpression(R"(\b\d{1,3}(\.\d{1,3}){3}\b)"),
                                        "<b style=\"color:#50fa7b\">\\1</b>");

                        // MAC — жёлтый
                        escaped.replace(QRegularExpression(R"([0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5})"),
                                        "<span style=\"color:#f1fa8c\">\\1</span>");

                        // Производитель (всё после двух табов)
                        escaped.replace(QRegularExpression(R"(\t\t.*)"),
                                        "<span style=\"color:#bd93f9\">\\1</span>");

                        m_arpTextEdit->append(escaped + "<br>");
                    }
                }

                if (!error.isEmpty()) {
                    m_arpTextEdit->append("<b style=\"color:#ff5555\">[ОШИБКА]</b><br>");
                    m_arpTextEdit->append("<span style=\"color:#ff6e6e\">" +
                                          error.toHtmlEscaped().replace("\n", "<br>") + "</span>");
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

// ======================== Остальной твой код (без изменений) ========================

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

void MainWindow::on_pushButton_start_clicked()
{
    QString vmName = getVmName().trimmed();
    if (vmName.isEmpty()) {
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
    ui->lineEdit->setText(normalized);

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

void MainWindow::startBhyve()
{
    QString vmName = getVmName();
    QString specialDiskPath = QString("/ntfs-2TB/vm/%1/%1.img").arg(vmName);
    QString diskPath;
    if (QFileInfo::exists(specialDiskPath)) {
        diskPath = specialDiskPath;
    } else {
        diskPath = getDiskPath();
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
    bhyveProcess->setProgram("doas");
    bhyveProcess->setArguments(QStringList() << "bhyve" << args);
    bhyveProcess->start();
    QTimer::singleShot(800, this, &MainWindow::attachTapToBridge);
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
    ui->pushButton_start->setText(running ? "Запускается..." : "Start VM");
    // ← ЭТА СТРОКА НОВАЯ: показываем кнопку только когда ВМ запущена
    ui->pushButton_closeWithoutKilling->setVisible(running);
}


void MainWindow::setVmStoppedState()
{
    ui->pushButton_start->setEnabled(true);
    ui->pushButton_stop->setEnabled(false);
    ui->pushButton_start->setText("Start VM");
    // ← ЭТА СТРОКА НОВАЯ: скрываем кнопку, когда ВМ остановлена
    ui->pushButton_closeWithoutKilling->setVisible(false);
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

void MainWindow::on_pushButton_closeWithoutKilling_clicked()
{
    if (bhyveProcess->state() != QProcess::Running) {
        close();
        return;
    }

    auto reply = QMessageBox::question(this,
                                       "Закрыть окно?",
                                       "<b>Виртуальная машина продолжит работать в фоне!</b><br><br>"
                                       "Остановить её потом можно вручную:<br><br>"
                                       "<code>doas pkill -f \"bhyve.*" + getVmName() + "\"</code><br>"
                                                           "или<br>"
                                                           "<code>doas bhyvectl --destroy --vm=" + getVmName() + "</code>",
                                       QMessageBox::Yes | QMessageBox::No,
                                       QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        ui->textEdit->append("<font color=\"#ffb86c\"><b>[GUI]</b> Окно закрывается — ВМ остаётся запущенной</font>");

        // Отсоединяем процесс, чтобы он не умер при закрытии окна
        if (bhyveProcess->state() == QProcess::Running) {
            bhyveProcess->setParent(nullptr);
            disconnect(bhyveProcess, nullptr, nullptr, nullptr);
        }

        close();  // или QApplication::quit();
    }
}


// ======================== Запущенные ВМ — точный фильтр ========================
void MainWindow::on_pushButton_runningVms_clicked()
{
    ui->pushButton_runningVms->setEnabled(false);
    showRunningVmsDialog();
}

void MainWindow::showRunningVmsDialog()
{
    if (!m_runningVmsDialog) {
        m_runningVmsDialog = new QDialog(this);
        m_runningVmsDialog->setWindowTitle("Запущенные виртуальные машины (bhyve)");
        m_runningVmsDialog->resize(1000, 680);
        m_runningVmsDialog->setModal(false);

        auto *layout = new QVBoxLayout(m_runningVmsDialog);

        // === Список ВМ вместо QTextEdit ===
        m_runningVmsList = new QListWidget(m_runningVmsDialog);
        m_runningVmsList->setStyleSheet(
            "QListWidget { background-color: #1e1e1e; color: #d4d4d4; font-family: Monospace; font-size: 13px; }"
            "QListWidget::item { padding: 8px; border-bottom: 1px solid #333; }"
            "QListWidget::item:selected { background: #44475a; }"
            );
        layout->addWidget(m_runningVmsList);

        // === Кнопки управления ===
        auto *btnLayout = new QHBoxLayout();

        auto *refreshBtn = new QPushButton("Обновить", m_runningVmsDialog);
        m_stopBtn = new QPushButton("Остановить ВМ", m_runningVmsDialog);
        m_killBtn = new QPushButton("Убить (kill -9)", m_runningVmsDialog);
        auto *copyBtn = new QPushButton("Копировать всё", m_runningVmsDialog);
        auto *closeBtn = new QPushButton("Закрыть", m_runningVmsDialog);

        m_stopBtn->setStyleSheet("background-color: #ff5555; color: white; font-weight: bold;");
        m_killBtn->setStyleSheet("background-color: #ff2d55; color: white; font-weight: bold;");
        m_stopBtn->setEnabled(false);
        m_killBtn->setEnabled(false);

        btnLayout->addWidget(refreshBtn);
        btnLayout->addWidget(m_stopBtn);
        btnLayout->addWidget(m_killBtn);
        btnLayout->addStretch();
        btnLayout->addWidget(copyBtn);
        btnLayout->addWidget(closeBtn);
        layout->addLayout(btnLayout);

        connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::on_pushButton_runningVms_clicked);
        connect(m_runningVmsList, &QListWidget::itemSelectionChanged, this, [this]() {
            bool hasSelection = !m_runningVmsList->selectedItems().isEmpty();
            m_stopBtn->setEnabled(hasSelection);
            m_killBtn->setEnabled(hasSelection);
        });
        connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::stopSelectedVm);
        connect(m_killBtn, &QPushButton::clicked, this, &MainWindow::killSelectedVm);
        connect(copyBtn, &QPushButton::clicked, this, [this]() {
            QString text;
            for (int i = 0; i < m_runningVmsList->count(); ++i)
                text += m_runningVmsList->item(i)->text() + "\n";
            qApp->clipboard()->setText(text.trimmed());
            QMessageBox::information(m_runningVmsDialog, "Готово", "Список скопирован в буфер");
        });
        connect(closeBtn, &QPushButton::clicked, m_runningVmsDialog, &QDialog::close);
        connect(m_runningVmsDialog, &QDialog::finished, this, [this]() {
            ui->pushButton_runningVms->setEnabled(true);
        });
    }

    m_runningVmsList->clear();
    m_runningVmsList->addItem("Поиск запущенных ВМ...");

    QProcess *ps = new QProcess(this);
    connect(ps, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, ps]() {
        QString out = ps->readAllStandardOutput();
        m_runningVmsList->clear();

        if (out.trimmed().isEmpty()) {
            m_runningVmsList->addItem("Нет запущенных bhyve-машин");
            ps->deleteLater();
            return;
        }

        QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (QString line : lines) {
            // Выделяем имя ВМ (последнее слово в строке)
            QString vmName = line.split(' ', Qt::SkipEmptyParts).last();

            // Красивая подсветка
            QString html = line.toHtmlEscaped();
            html.replace(QRegularExpression(R"(^\w+\s+\w+\s+)"), "<b style=\"color:#50fa7b\">"); // PID
            html.replace(QRegularExpression(R"(\s+\d+\.\d+\s+)"), "</b> <span style=\"color:#ffb86c\">"); // CPU
            html.replace(QRegularExpression(R"(\s+\d+[KMG]\s+)"), "</span> <span style=\"color:#f1fa8c\">"); // MEM
            html.replace(QRegularExpression(R"(([^ ]+)$)"), "</span> <b style=\"color:#bd93f9\">\\1</b>");

            auto *item = new QListWidgetItem(html);
            item->setData(Qt::UserRole, vmName); // сохраняем имя ВМ
            m_runningVmsList->addItem(item);
        }
        ps->deleteLater();
    });

    ps->setProgram("doas");
    ps->setArguments({"sh", "-c", "ps auxww | grep -E 'bhyve: .* \\(bhyve\\)' | grep -v grep"});
    ps->start();

    m_runningVmsDialog->show();
    m_runningVmsDialog->raise();
    m_runningVmsDialog->activateWindow();
}


void MainWindow::stopSelectedVm()
{
    auto *item = m_runningVmsList->currentItem();
    if (!item) return;

    QString vmName = item->data(Qt::UserRole).toString();

    auto reply = QMessageBox::question(m_runningVmsDialog, "Остановить ВМ?",
                                       QString("Грациозно остановить виртуальную машину <b>%1</b>?").arg(vmName),
                                       QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        QProcess::execute("doas", {"bhyvectl", "--vm=" + vmName, "--force-poweroff"});
        QTimer::singleShot(1500, this, &MainWindow::on_pushButton_runningVms_clicked); // обновить список
    }
}

void MainWindow::killSelectedVm()
{
    auto *item = m_runningVmsList->currentItem();
    if (!item) return;

    QString vmName = item->data(Qt::UserRole).toString();

    auto reply = QMessageBox::question(m_runningVmsDialog, "УБИТЬ ВМ?",
                                       QString("<font color=\"#ff5555\"><b>Жёстко убить (kill -9) ВМ %1?</b></font><br>"
                                               "Это может повредить файловую систему гостя!").arg(vmName),
                                       QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        QProcess::execute("doas", {"sh", "-c", QString("pkill -9 -f \"bhyve: %1 \"").arg(vmName)});
        QTimer::singleShot(1500, this, &MainWindow::on_pushButton_runningVms_clicked);
    }
}

QString MainWindow::getVmName() const { return ui->lineEdit_2->text().trimmed(); }
QString MainWindow::getMemory() const { return ui->lineEdit->text().trimmed(); }
QString MainWindow::getDiskPath() const { return ui->lineEdit_3->text().trimmed(); }
QString MainWindow::getIsoPath() const { return ui->lineEdit_4->text().trimmed(); }
QString MainWindow::getTapInterface() const { return ui->lineEdit_5->text().trimmed(); }
