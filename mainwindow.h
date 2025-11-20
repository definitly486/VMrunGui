#pragma once

#include <QMainWindow>
#include <QProcess>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_pushButton_start_clicked();
    void on_pushButton_stop_clicked();

    void onVmFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onVmReadyReadStandardOutput();
    void onVmReadyReadStandardError();
    void onVmErrorOccurred(QProcess::ProcessError error);
    void cleanupAllTapDevices();
    void startBhyve();
    void destroyVm();
    void setVmRunningState(bool running);
    void setVmStoppedState();
    void attachTapToBridge();
    void removeTapFromBridge();     // <-- ЭТУ СТРОКУ ДОБАВЬ


private:
    Ui::MainWindow *ui;
    QProcess *bhyveProcess = nullptr;
    bool shouldRestart = true;

    QString getVmName() const;
    QString getMemory() const;
    QString getDiskPath() const;
    QString getIsoPath() const;
    QString getTapInterface() const;
};
