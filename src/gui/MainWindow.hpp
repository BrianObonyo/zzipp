#pragma once
#include <QElapsedTimer>
#include <QMainWindow>
#include <atomic>
#include <functional>
#include <memory>

#include "archive.hpp"
#include "commands.hpp"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QThread;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class DropList;

// Main window of the zzipp desktop app. All archive work runs on a worker thread through
// the same engine functions the command-line tool uses (zzipp::cmd_*).
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    enum Page { PageCreate = 0, PageBrowse = 1, PageSettings = 2 };

    MainWindow();
    ~MainWindow() override;

    // Programmatic API: used by drag & drop, the command line, and the self-test.
    void setInteractive(bool on) { interactive_ = on; }  // false: no modal dialogs
    void showPage(int page);
    void addInputs(const QStringList& paths);
    void clearInputs();
    void setOutputPath(const QString& path);
    void setPreset(int index);  // 0 fast, 1 balanced, 2 maximum
    void startCreate();
    void openArchive(const QString& path);
    void startExtract(const QString& dest, const QStringList& selectors, bool overwrite);
    void startTest();
    void startExportZip(const QString& zipPath);
    int topLevelCount() const;
    bool busy() const { return busy_; }
    void requestCancel();

signals:
    void taskFinished(bool ok, int rc, const QString& message);

protected:
    void closeEvent(QCloseEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    struct CreateParams {
        uint32_t preset = 6;
        uint64_t block = 0;
        uint32_t dict = 0;
        unsigned threads = 1;
        uint64_t memPerThread = 0;
    };

    QWidget* buildSidebar();
    QWidget* buildCreatePage();
    QWidget* buildBrowsePage();
    QWidget* buildSettingsPage();
    QWidget* buildTaskPanel();

    CreateParams createParams() const;
    void updateMemoryHint();
    void populateTree();
    void updateSummary();
    void applyFilter(const QString& text);
    QStringList selectedPaths() const;
    void updateBrowseButtons();

    void runTask(const QString& title, std::function<int(const zzipp::Callbacks&)> work,
                 std::function<void(bool ok, int rc, const QString& err)> done);
    void setBusyUi(bool busy, const QString& title = QString());
    void appendLog(const QString& line);
    void tick();
    void notify(int icon, const QString& title, const QString& text);
    void loadSettings();
    void saveSettings();
    QString pickDir(const QString& caption, const QString& start);

    QStackedWidget* stack_ = nullptr;
    QWidget* sidebar_ = nullptr;
    QButtonGroup* navGroup_ = nullptr;

    // create page
    DropList* inputs_ = nullptr;
    QLineEdit* outEdit_ = nullptr;
    QComboBox *presetCombo_ = nullptr, *blockCombo_ = nullptr, *dictCombo_ = nullptr;
    QCheckBox *dedupCheck_ = nullptr, *sortCheck_ = nullptr;
    QLabel *presetHint_ = nullptr, *memHint_ = nullptr;
    QPushButton* createBtn_ = nullptr;

    // browse page
    QLabel *archiveLabel_ = nullptr, *sumFiles_ = nullptr, *sumSize_ = nullptr, *sumPacked_ = nullptr, *sumRatio_ = nullptr;
    QLineEdit *filterEdit_ = nullptr, *destEdit_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QCheckBox* overwriteCheck_ = nullptr;
    QPushButton *extractSelBtn_ = nullptr, *extractAllBtn_ = nullptr, *testBtn_ = nullptr, *zipBtn_ = nullptr;

    // settings page
    QSpinBox* threadsSpin_ = nullptr;
    QCheckBox* overwriteDefault_ = nullptr;

    // task panel
    QLabel *statusLabel_ = nullptr, *elapsedLabel_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton *cancelBtn_ = nullptr, *detailsBtn_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QTimer* timer_ = nullptr;

    std::unique_ptr<zzipp::ArchiveReader> reader_;
    QString archivePath_;
    QString lastDir_;

    QThread* worker_ = nullptr;
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> progDone_{0}, progTotal_{0};
    QElapsedTimer elapsed_;
    bool busy_ = false;
    bool interactive_ = true;
};
