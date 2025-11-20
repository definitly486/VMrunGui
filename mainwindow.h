#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QProcess>

// Эти два include обязательны!
#include <QDialog>
#include <QTextEdit>
#include <QListWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_pushButton_start_clicked();
    void on_pushButton_stop_clicked();
    void cleanupAllTapDevices();
    void on_pushButton_arpScan_clicked();
 void on_pushButton_runningVms_clicked();
    void stopSelectedVm();
    void killSelectedVm();
   void on_pushButton_killAll_clicked();  // ← вот эта строка решает ошибку компиляции
private:
    void startBhyve();
    void attachTapToBridge();
    void removeTapFromBridge();
    void setVmRunningState(bool running);
    void setVmStoppedState();
    void onVmFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onVmErrorOccurred(QProcess::ProcessError error);
    void destroyVm();
    void onVmReadyReadStandardOutput();
    void onVmReadyReadStandardError();
void on_pushButton_closeWithoutKilling_clicked();
    bool validateMemory(const QString &mem, QString *errorMessage = nullptr);

QListWidget *m_runningVmsList = nullptr;
QPushButton *m_stopBtn = nullptr;
QPushButton *m_killBtn = nullptr;

QDialog *m_runningVmsDialog = nullptr;
QTextEdit *m_runningVmsTextEdit = nullptr;

void showRunningVmsDialog();

    // Геттеры
    QString getVmName() const;
    QString getMemory() const;
    QString getDiskPath() const;
    QString getIsoPath() const;
    QString getTapInterface() const;

    void showArpScanDialog();  // ← объявление функции

    // ← ВАЖНО: без инициализации = nullptr прямо в заголовке!
    QDialog   *m_arpDialog;
    QTextEdit *m_arpTextEdit;

    Ui::MainWindow *ui;
    QProcess *bhyveProcess;
    bool shouldRestart;
};

#endif // MAINWINDOW_H
