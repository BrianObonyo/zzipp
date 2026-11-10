#include "MainWindow.hpp"

#include <lzma.h>

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <unordered_map>

#include "util.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef ZZIPP_VERSION
#define ZZIPP_VERSION "dev"
#endif

namespace {

std::string u8(const QString& s) { return s.toUtf8().toStdString(); }
QString q(const std::string& s) { return QString::fromUtf8(s.data(), static_cast<int>(s.size())); }
QString humanSize(uint64_t b) { return q(zzipp::human_size(b)); }

QString methodName(zzipp::Method m) {
    switch (m) {
        case zzipp::Method::Store: return QStringLiteral("Stored");
        case zzipp::Method::Lzma2: return QStringLiteral("LZMA2");
        case zzipp::Method::Lzma2Bcj86: return QStringLiteral("BCJ + LZMA2");
    }
    return {};
}

quint64 totalRam() {
#ifdef _WIN32
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES), sz = sysconf(_SC_PAGE_SIZE);
    return (pages > 0 && sz > 0) ? quint64(pages) * quint64(sz) : 0;
#endif
}

QString formatDuration(qint64 ms) {
    qint64 s = ms / 1000;
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QFrame* card() {
    auto* f = new QFrame;
    f->setObjectName("card");
    return f;
}

QLabel* label(const QString& text, const char* objectName = nullptr) {
    auto* l = new QLabel(text);
    if (objectName) l->setObjectName(objectName);
    return l;
}

}  // namespace

// A list that accepts files/folders dragged in from the file manager.
class DropList : public QListWidget {
public:
    std::function<void(const QStringList&)> onDrop;
    explicit DropList(QWidget* parent = nullptr) : QListWidget(parent) {
        setAcceptDrops(true);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setAlternatingRowColors(false);
    }

protected:
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override {
        QStringList paths;
        for (const QUrl& u : e->mimeData()->urls())
            if (u.isLocalFile()) paths << u.toLocalFile();
        if (!paths.isEmpty() && onDrop) {
            onDrop(paths);
            e->acceptProposedAction();
        }
    }
    void paintEvent(QPaintEvent* e) override {
        QListWidget::paintEvent(e);
        if (count() == 0) {
            QPainter p(viewport());
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(viewport()->rect(), Qt::AlignCenter,
                       tr("Drop files or folders here\nor use the buttons below"));
        }
    }
};

namespace {

class EntryItem : public QTreeWidgetItem {
public:
    bool isDir = false;
    bool operator<(const QTreeWidgetItem& other) const override {
        const auto* o = static_cast<const EntryItem*>(&other);
        QTreeWidget* tw = treeWidget();
        const int col = tw ? tw->sortColumn() : 0;
        const bool desc = tw && tw->header()->sortIndicatorOrder() == Qt::DescendingOrder;
        if (isDir != o->isDir) return desc ? !isDir : isDir;  // folders stay on top
        if (col == 1 || col == 2) return data(col, Qt::UserRole).toLongLong() < o->data(col, Qt::UserRole).toLongLong();
        return text(col).compare(o->text(col), Qt::CaseInsensitive) < 0;
    }
};

const uint32_t kPresetDict[10] = {256u << 10, 1u << 20, 2u << 20, 4u << 20, 4u << 20,
                                  8u << 20,   8u << 20, 16u << 20, 32u << 20, 64u << 20};

}  // namespace

// ============================================================================================

MainWindow::MainWindow() {
    setWindowTitle("zzipp");
    resize(1040, 720);
    setMinimumSize(900, 600);
    setAcceptDrops(true);

    auto* root = new QWidget;
    root->setObjectName("root");
    setCentralWidget(root);
    auto* h = new QHBoxLayout(root);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    sidebar_ = buildSidebar();
    h->addWidget(sidebar_);

    auto* right = new QWidget;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(28, 24, 28, 16);
    rv->setSpacing(14);
    stack_ = new QStackedWidget;
    stack_->addWidget(buildCreatePage());
    stack_->addWidget(buildBrowsePage());
    stack_->addWidget(buildSettingsPage());
    rv->addWidget(stack_, 1);
    rv->addWidget(buildTaskPanel());
    h->addWidget(right, 1);

    timer_ = new QTimer(this);
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, [this] { tick(); });

    loadSettings();
    updateMemoryHint();
    updateBrowseButtons();
    showPage(PageCreate);
}

MainWindow::~MainWindow() = default;

QWidget* MainWindow::buildSidebar() {
    auto* w = new QWidget;
    w->setObjectName("sidebar");
    w->setFixedWidth(220);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(16, 22, 16, 16);
    v->setSpacing(6);

    auto* brand = new QHBoxLayout;
    auto* logo = new QLabel;
    logo->setPixmap(windowIcon().pixmap(34, 34));
    brand->addWidget(logo);
    brand->addWidget(label("zzipp", "appName"));
    brand->addStretch();
    v->addLayout(brand);
    v->addSpacing(18);

    navGroup_ = new QButtonGroup(this);
    navGroup_->setExclusive(true);
    const char* names[] = {"Create archive", "Open && extract", "Settings"};
    for (int i = 0; i < 3; ++i) {
        auto* b = new QPushButton(tr(names[i]));
        b->setObjectName("nav");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        navGroup_->addButton(b, i);
        v->addWidget(b);
    }
    connect(navGroup_, &QButtonGroup::idClicked, this, [this](int id) { stack_->setCurrentIndex(id); });
    v->addStretch();
    v->addWidget(label(QString("v%1 · LZMA2 engine").arg(ZZIPP_VERSION), "muted"));
    return w;
}

QWidget* MainWindow::buildCreatePage() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(14);
    v->addWidget(label(tr("Create archive"), "h1"));
    v->addWidget(label(tr("Pack files and folders into one compact .mzg archive."), "sub"));

    // --- items card
    auto* c1 = card();
    auto* c1v = new QVBoxLayout(c1);
    c1v->setContentsMargins(16, 14, 16, 14);
    c1v->addWidget(label(tr("Items to archive"), "cardTitle"));
    inputs_ = new DropList;
    inputs_->setMinimumHeight(130);
    inputs_->onDrop = [this](const QStringList& p) { addInputs(p); };
    c1v->addWidget(inputs_, 1);
    auto* br = new QHBoxLayout;
    auto* addFiles = new QPushButton(tr("Add files…"));
    auto* addDir = new QPushButton(tr("Add folder…"));
    auto* rem = new QPushButton(tr("Remove selected"));
    auto* clear = new QPushButton(tr("Clear"));
    for (auto* b : {addFiles, addDir, rem, clear}) br->addWidget(b);
    br->addStretch();
    c1v->addLayout(br);
    v->addWidget(c1, 1);

    connect(addFiles, &QPushButton::clicked, this, [this] {
        const QStringList f = QFileDialog::getOpenFileNames(this, tr("Add files"), lastDir_);
        if (!f.isEmpty()) addInputs(f);
    });
    connect(addDir, &QPushButton::clicked, this, [this] {
        const QString d = pickDir(tr("Add folder"), lastDir_);
        if (!d.isEmpty()) addInputs({d});
    });
    connect(rem, &QPushButton::clicked, this, [this] { qDeleteAll(inputs_->selectedItems()); });
    connect(clear, &QPushButton::clicked, this, [this] { inputs_->clear(); });

    // --- options card
    auto* c2 = card();
    auto* g = new QGridLayout(c2);
    g->setContentsMargins(16, 14, 16, 14);
    g->setHorizontalSpacing(12);
    g->setVerticalSpacing(10);
    g->setColumnStretch(1, 1);

    g->addWidget(label(tr("Save as"), "fieldLabel"), 0, 0);
    outEdit_ = new QLineEdit;
    outEdit_->setPlaceholderText(tr("Choose where to save the archive"));
    auto* browseOut = new QPushButton(tr("Browse…"));
    g->addWidget(outEdit_, 0, 1);
    g->addWidget(browseOut, 0, 2);
    connect(browseOut, &QPushButton::clicked, this, [this] {
        QString f = QFileDialog::getSaveFileName(this, tr("Save archive"),
                                                 outEdit_->text().isEmpty() ? lastDir_ : outEdit_->text(),
                                                 tr("zzipp archive (*.mzg)"));
        if (!f.isEmpty()) {
            if (QFileInfo(f).suffix().isEmpty()) f += ".mzg";
            outEdit_->setText(f);
        }
    });

    g->addWidget(label(tr("Compression"), "fieldLabel"), 1, 0);
    presetCombo_ = new QComboBox;
    presetCombo_->addItems({tr("Fast"), tr("Balanced (recommended)"), tr("Maximum")});
    presetCombo_->setCurrentIndex(1);
    g->addWidget(presetCombo_, 1, 1);
    presetHint_ = label("", "muted");
    presetHint_->setWordWrap(true);
    g->addWidget(presetHint_, 2, 1, 1, 2);

    g->addWidget(label(tr("Advanced"), "fieldLabel"), 3, 0);
    auto* adv = new QHBoxLayout;
    adv->setSpacing(8);
    adv->addWidget(label(tr("Block size"), "muted"));
    blockCombo_ = new QComboBox;
    blockCombo_->addItem(tr("Auto"), 0);
    for (int mb : {16, 32, 64, 128, 256, 512}) blockCombo_->addItem(QString("%1 MiB").arg(mb), mb);
    adv->addWidget(blockCombo_);
    adv->addSpacing(12);
    adv->addWidget(label(tr("Dictionary"), "muted"));
    dictCombo_ = new QComboBox;
    dictCombo_->addItem(tr("Auto"), 0);
    for (int mb : {8, 16, 32, 64, 128}) dictCombo_->addItem(QString("%1 MiB").arg(mb), mb);
    adv->addWidget(dictCombo_);
    adv->addStretch();
    g->addLayout(adv, 3, 1, 1, 2);

    auto* chk = new QHBoxLayout;
    chk->setSpacing(28);
    dedupCheck_ = new QCheckBox(tr("Store identical files once"));
    dedupCheck_->setChecked(true);
    sortCheck_ = new QCheckBox(tr("Group similar files together"));
    sortCheck_->setChecked(true);
    chk->addWidget(dedupCheck_);
    chk->addWidget(sortCheck_);
    chk->addStretch();
    g->addLayout(chk, 4, 1, 1, 2);
    memHint_ = label("", "muted");
    memHint_->setWordWrap(true);
    g->addWidget(memHint_, 5, 1, 1, 2);
    v->addWidget(c2);

    for (auto* cb : {presetCombo_, blockCombo_, dictCombo_})
        connect(cb, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { updateMemoryHint(); });

    auto* row = new QHBoxLayout;
    row->addStretch();
    createBtn_ = new QPushButton(tr("Create archive"));
    createBtn_->setObjectName("primary");
    createBtn_->setCursor(Qt::PointingHandCursor);
    row->addWidget(createBtn_);
    v->addLayout(row);
    connect(createBtn_, &QPushButton::clicked, this, [this] { startCreate(); });
    return page;
}

QWidget* MainWindow::buildBrowsePage() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(12);

    auto* top = new QHBoxLayout;
    auto* titles = new QVBoxLayout;
    titles->addWidget(label(tr("Open & extract"), "h1"));
    archiveLabel_ = label(tr("No archive open. Open a .mzg file or drop one onto this window."), "sub");
    titles->addWidget(archiveLabel_);
    top->addLayout(titles, 1);
    auto* openBtn = new QPushButton(tr("Open archive…"));
    openBtn->setObjectName("primary");
    top->addWidget(openBtn, 0, Qt::AlignTop);
    v->addLayout(top);
    connect(openBtn, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open archive"), lastDir_,
                                                       tr("zzipp archive (*.mzg);;All files (*)"));
        if (!f.isEmpty()) openArchive(f);
    });

    // summary cards
    auto* sum = new QHBoxLayout;
    sum->setSpacing(10);
    auto addStat = [&](QLabel*& value, const QString& title) {
        auto* c = card();
        auto* cv = new QVBoxLayout(c);
        cv->setContentsMargins(14, 10, 14, 10);
        cv->setSpacing(0);
        cv->addWidget(label(title, "muted"));
        value = label("—", "stat");
        cv->addWidget(value);
        sum->addWidget(c, 1);
    };
    addStat(sumFiles_, tr("Contents"));
    addStat(sumSize_, tr("Original size"));
    addStat(sumPacked_, tr("Archive size"));
    addStat(sumRatio_, tr("Compression"));
    v->addLayout(sum);

    filterEdit_ = new QLineEdit;
    filterEdit_->setPlaceholderText(tr("Filter by name…"));
    filterEdit_->setClearButtonEnabled(true);
    v->addWidget(filterEdit_);
    connect(filterEdit_, &QLineEdit::textChanged, this, [this](const QString& t) { applyFilter(t); });

    tree_ = new QTreeWidget;
    tree_->setColumnCount(5);
    tree_->setHeaderLabels({tr("Name"), tr("Size"), tr("Modified"), tr("CRC-32"), tr("Compression (block)")});
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setUniformRowHeights(true);
    tree_->setRootIsDecorated(true);
    tree_->setSortingEnabled(true);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    v->addWidget(tree_, 1);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] { updateBrowseButtons(); });

    // extraction controls
    auto* c = card();
    auto* g = new QGridLayout(c);
    g->setContentsMargins(14, 12, 14, 12);
    g->setHorizontalSpacing(10);
    g->setColumnStretch(1, 1);
    g->addWidget(label(tr("Extract to"), "fieldLabel"), 0, 0);
    destEdit_ = new QLineEdit;
    destEdit_->setPlaceholderText(tr("Destination folder"));
    auto* browseDest = new QPushButton(tr("Browse…"));
    g->addWidget(destEdit_, 0, 1);
    g->addWidget(browseDest, 0, 2);
    overwriteCheck_ = new QCheckBox(tr("Overwrite existing files"));
    g->addWidget(overwriteCheck_, 1, 1);
    auto* btns = new QHBoxLayout;
    extractSelBtn_ = new QPushButton(tr("Extract selected"));
    extractAllBtn_ = new QPushButton(tr("Extract all"));
    extractAllBtn_->setObjectName("primary");
    testBtn_ = new QPushButton(tr("Test integrity"));
    zipBtn_ = new QPushButton(tr("Export as ZIP…"));
    btns->addWidget(testBtn_);
    btns->addWidget(zipBtn_);
    btns->addStretch();
    btns->addWidget(extractSelBtn_);
    btns->addWidget(extractAllBtn_);
    g->addLayout(btns, 2, 0, 1, 3);
    v->addWidget(c);

    connect(browseDest, &QPushButton::clicked, this, [this] {
        const QString d = pickDir(tr("Extract to"), destEdit_->text().isEmpty() ? lastDir_ : destEdit_->text());
        if (!d.isEmpty()) destEdit_->setText(d);
    });
    auto destOrWarn = [this]() -> QString {
        const QString d = destEdit_->text().trimmed();
        if (d.isEmpty()) notify(QMessageBox::Warning, tr("No destination"), tr("Choose a destination folder first."));
        return d;
    };
    connect(extractAllBtn_, &QPushButton::clicked, this, [=] {
        const QString d = destOrWarn();
        if (!d.isEmpty()) startExtract(d, {}, overwriteCheck_->isChecked());
    });
    connect(extractSelBtn_, &QPushButton::clicked, this, [=] {
        const QString d = destOrWarn();
        if (!d.isEmpty()) startExtract(d, selectedPaths(), overwriteCheck_->isChecked());
    });
    connect(testBtn_, &QPushButton::clicked, this, [this] { startTest(); });
    connect(zipBtn_, &QPushButton::clicked, this, [this] {
        if (!reader_) return;
        QFileInfo fi(archivePath_);
        const QString f = QFileDialog::getSaveFileName(this, tr("Export as ZIP"),
                                                       fi.absolutePath() + "/" + fi.completeBaseName() + ".zip",
                                                       tr("ZIP archive (*.zip)"));
        if (!f.isEmpty()) startExportZip(f);
    });
    return page;
}

QWidget* MainWindow::buildSettingsPage() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(14);
    v->addWidget(label(tr("Settings"), "h1"));
    v->addWidget(label(tr("Preferences are saved automatically."), "sub"));

    auto* c = card();
    auto* g = new QGridLayout(c);
    g->setContentsMargins(16, 14, 16, 14);
    g->setHorizontalSpacing(12);
    g->setVerticalSpacing(12);
    g->setColumnStretch(2, 1);
    g->addWidget(label(tr("Worker threads"), "fieldLabel"), 0, 0);
    threadsSpin_ = new QSpinBox;
    threadsSpin_->setRange(1, 64);
    threadsSpin_->setValue(int(zzipp::default_threads()));
    g->addWidget(threadsSpin_, 0, 1);
    g->addWidget(label(tr("Used for creating, extracting and testing. More threads are faster but use more memory."), "muted"), 0, 2);
    overwriteDefault_ = new QCheckBox(tr("Overwrite existing files when extracting by default"));
    g->addWidget(overwriteDefault_, 1, 0, 1, 3);
    v->addWidget(c);
    connect(threadsSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { updateMemoryHint(); });
    connect(overwriteDefault_, &QCheckBox::toggled, this, [this](bool on) { overwriteCheck_->setChecked(on); });

    auto* about = card();
    auto* av = new QVBoxLayout(about);
    av->setContentsMargins(16, 14, 16, 14);
    av->addWidget(label(tr("About"), "cardTitle"));
    auto* t = label(tr("zzipp %1 — solid-compression archiver.\nEngine: LZMA2 (liblzma %2), deduplication, parallel blocks, ZIP export.\n"
                       "Licensed under the Apache License 2.0. This app uses the Qt toolkit.")
                        .arg(ZZIPP_VERSION, lzma_version_string()), "muted");
    t->setWordWrap(true);
    av->addWidget(t);
    v->addWidget(about);
    v->addStretch();
    return page;
}

QWidget* MainWindow::buildTaskPanel() {
    auto* c = card();
    auto* v = new QVBoxLayout(c);
    v->setContentsMargins(14, 10, 14, 10);
    v->setSpacing(8);
    auto* r = new QHBoxLayout;
    statusLabel_ = label(tr("Ready"), "status");
    r->addWidget(statusLabel_, 1);
    elapsedLabel_ = label("", "muted");
    r->addWidget(elapsedLabel_);
    detailsBtn_ = new QPushButton(tr("Details"));
    detailsBtn_->setCheckable(true);
    r->addWidget(detailsBtn_);
    cancelBtn_ = new QPushButton(tr("Cancel"));
    cancelBtn_->setObjectName("danger");
    cancelBtn_->setEnabled(false);
    r->addWidget(cancelBtn_);
    v->addLayout(r);
    progress_ = new QProgressBar;
    progress_->setTextVisible(false);
    progress_->setFixedHeight(10);
    progress_->setRange(0, 1000);
    v->addWidget(progress_);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(2000);
    log_->setFixedHeight(120);
    log_->setVisible(false);
    v->addWidget(log_);
    connect(detailsBtn_, &QPushButton::toggled, this, [this](bool on) { log_->setVisible(on); });
    connect(cancelBtn_, &QPushButton::clicked, this, [this] { requestCancel(); });
    return c;
}

void MainWindow::requestCancel() {
    if (!busy_) return;
    cancel_ = true;
    cancelBtn_->setEnabled(false);
    statusLabel_->setText(tr("Cancelling… waiting for the current block to finish"));
}

// ---------------------------------------------------------------------------- create flow ----

void MainWindow::showPage(int page) {
    stack_->setCurrentIndex(page);
    if (auto* b = navGroup_->button(page)) b->setChecked(true);
}

QString MainWindow::pickDir(const QString& caption, const QString& start) {
    const QString d = QFileDialog::getExistingDirectory(this, caption, start);
    if (!d.isEmpty()) lastDir_ = d;
    return d;
}

void MainWindow::addInputs(const QStringList& paths) {
    for (const QString& p : paths) {
        const QString clean = QDir::toNativeSeparators(QFileInfo(p).absoluteFilePath());
        bool dup = false;
        for (int i = 0; i < inputs_->count(); ++i)
            if (inputs_->item(i)->data(Qt::UserRole).toString() == clean) dup = true;
        if (dup) continue;
        auto* it = new QListWidgetItem(QFileInfo(p).isDir() ? style()->standardIcon(QStyle::SP_DirIcon)
                                                            : style()->standardIcon(QStyle::SP_FileIcon),
                                       clean);
        it->setData(Qt::UserRole, clean);
        inputs_->addItem(it);
    }
    if (outEdit_->text().isEmpty() && inputs_->count() > 0) {
        QFileInfo fi(inputs_->item(0)->data(Qt::UserRole).toString());
        const QString base = inputs_->count() == 1 ? fi.completeBaseName().isEmpty() ? fi.fileName() : fi.completeBaseName()
                                                   : QStringLiteral("archive");
        outEdit_->setText(QDir::toNativeSeparators(fi.absolutePath() + "/" + base + ".mzg"));
    }
    if (!paths.isEmpty()) lastDir_ = QFileInfo(paths.first()).absolutePath();
}

void MainWindow::clearInputs() { inputs_->clear(); }
void MainWindow::setOutputPath(const QString& p) { outEdit_->setText(p); }
void MainWindow::setPreset(int i) { presetCombo_->setCurrentIndex(i); }

MainWindow::CreateParams MainWindow::createParams() const {
    CreateParams p;
    const int idx = presetCombo_->currentIndex();
    const int level = idx == 0 ? 1 : idx == 1 ? 6 : 9;
    p.preset = uint32_t(level);
    if (idx == 2) p.preset |= LZMA_PRESET_EXTREME;
    const int blockMb = blockCombo_->currentData().toInt();
    p.block = blockMb ? uint64_t(blockMb) << 20 : (idx == 2 ? 128ull << 20 : 32ull << 20);
    const int dictMb = dictCombo_->currentData().toInt();
    p.dict = dictMb ? uint32_t(dictMb) << 20 : 0;
    const uint64_t dictEff = std::min<uint64_t>(p.dict ? p.dict : kPresetDict[level], p.block);
    p.memPerThread = 2 * p.block + 12 * dictEff;  // rough: raw block + output buffer + match finder
    unsigned t = unsigned(threadsSpin_ ? threadsSpin_->value() : 1);
    if (const quint64 ram = totalRam()) {
        const quint64 cap = std::max<quint64>(1, quint64(double(ram) * 0.6) / std::max<uint64_t>(1, p.memPerThread));
        t = unsigned(std::min<quint64>(t, cap));
    }
    p.threads = std::max(1u, t);
    return p;
}

void MainWindow::updateMemoryHint() {
    if (!memHint_) return;
    static const char* hints[] = {"Quickest. Files come out noticeably larger than the other presets.",
                                  "Good balance of speed and size for everyday use.",
                                  "Smallest files. Much slower and uses large blocks and dictionaries (more memory)."};
    presetHint_->setText(tr(hints[presetCombo_->currentIndex()]));
    const CreateParams p = createParams();
    memHint_->setText(tr("Estimated memory: about %1 per thread · using %2 thread(s)")
                          .arg(humanSize(p.memPerThread)).arg(p.threads) +
                      (unsigned(threadsSpin_->value()) > p.threads ? tr(" (limited by available memory)") : QString()));
}

void MainWindow::notify(int icon, const QString& title, const QString& text) {
    if (!interactive_) {
        appendLog(title + ": " + text);
        return;
    }
    QMessageBox box(QMessageBox::Icon(icon), title, text, QMessageBox::Ok, this);
    box.exec();
}

void MainWindow::startCreate() {
    if (busy_) return;
    if (inputs_->count() == 0) return notify(QMessageBox::Warning, tr("Nothing to archive"), tr("Add at least one file or folder."));
    QString out = outEdit_->text().trimmed();
    if (out.isEmpty()) return notify(QMessageBox::Warning, tr("No output file"), tr("Choose where to save the archive."));
    if (QFileInfo(out).suffix().isEmpty()) out += ".mzg";
    if (QFileInfo::exists(out) && interactive_) {
        if (QMessageBox::question(this, tr("Overwrite?"), tr("%1 already exists. Replace it?").arg(QFileInfo(out).fileName())) != QMessageBox::Yes)
            return;
    }
    const CreateParams p = createParams();
    zzipp::CreateOptions o;
    o.archive = u8(out);
    for (int i = 0; i < inputs_->count(); ++i) o.inputs.push_back(u8(inputs_->item(i)->data(Qt::UserRole).toString()));
    o.preset = p.preset;
    o.block = p.block;
    o.dict = p.dict;
    o.threads = p.threads;
    o.dedup = dedupCheck_->isChecked();
    o.sort = sortCheck_->isChecked();
    o.force = true;
    runTask(tr("Creating %1…").arg(QFileInfo(out).fileName()),
            [o](const zzipp::Callbacks& cb) mutable {
                o.cb = cb;
                return zzipp::cmd_create(o);
            },
            [this, out](bool ok, int, const QString& err) {
                if (!ok) {
                    if (!err.isEmpty()) notify(QMessageBox::Critical, tr("Could not create archive"), err);
                    return;
                }
                const QString text = tr("Saved %1 (%2).").arg(QFileInfo(out).fileName(), humanSize(uint64_t(QFileInfo(out).size())));
                if (!interactive_) return;
                QMessageBox box(QMessageBox::Information, tr("Archive created"), text, QMessageBox::NoButton, this);
                auto* browse = box.addButton(tr("Browse archive"), QMessageBox::AcceptRole);
                auto* show = box.addButton(tr("Show in folder"), QMessageBox::ActionRole);
                box.addButton(tr("Close"), QMessageBox::RejectRole);
                box.exec();
                if (box.clickedButton() == browse) openArchive(out);
                else if (box.clickedButton() == show) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(out).absolutePath()));
            });
}

// ---------------------------------------------------------------------------- browse flow ----

void MainWindow::openArchive(const QString& path) {
    try {
        auto r = std::make_unique<zzipp::ArchiveReader>(u8(path));
        reader_ = std::move(r);
    } catch (const std::exception& e) {
        notify(QMessageBox::Critical, tr("Cannot open archive"), tr("%1\n\n%2").arg(QFileInfo(path).fileName(), q(e.what())));
        return;
    }
    archivePath_ = QFileInfo(path).absoluteFilePath();
    lastDir_ = QFileInfo(path).absolutePath();
    archiveLabel_->setText(QDir::toNativeSeparators(archivePath_));
    if (destEdit_->text().isEmpty() || destEdit_->property("auto").toBool()) {
        destEdit_->setText(QDir::toNativeSeparators(lastDir_ + "/" + QFileInfo(path).completeBaseName()));
        destEdit_->setProperty("auto", true);
    }
    populateTree();
    updateSummary();
    updateBrowseButtons();
    appendLog(tr("Opened %1").arg(archivePath_));
    showPage(PageBrowse);
}

void MainWindow::populateTree() {
    tree_->setSortingEnabled(false);
    tree_->clear();
    if (!reader_) return;
    const auto& idx = reader_->index();
    std::unordered_map<std::string, EntryItem*> dirs;

    std::function<EntryItem*(const std::string&)> ensureDir = [&](const std::string& p) -> EntryItem* {
        if (p.empty()) return nullptr;
        auto it = dirs.find(p);
        if (it != dirs.end()) return it->second;
        const size_t slash = p.rfind('/');
        EntryItem* parent = ensureDir(slash == std::string::npos ? std::string() : p.substr(0, slash));
        auto* item = new EntryItem;
        item->isDir = true;
        if (parent) parent->addChild(item);
        else tree_->addTopLevelItem(item);
        item->setText(0, q(slash == std::string::npos ? p : p.substr(slash + 1)));
        item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        item->setData(0, Qt::UserRole, q(p));
        item->setData(1, Qt::UserRole, 0);
        item->setData(2, Qt::UserRole, 0);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        dirs[p] = item;
        return item;
    };

    for (const zzipp::Entry& e : idx.entries) {
        const size_t slash = e.path.rfind('/');
        const std::string parentPath = slash == std::string::npos ? std::string() : e.path.substr(0, slash);
        if (e.kind == zzipp::EntryKind::Dir) {
            EntryItem* d = ensureDir(e.path);
            d->setData(2, Qt::UserRole, qlonglong(e.mtime));
            d->setText(2, QDateTime::fromSecsSinceEpoch(e.mtime).toString("yyyy-MM-dd HH:mm"));
            continue;
        }
        EntryItem* parent = ensureDir(parentPath);
        auto* item = new EntryItem;
        if (parent) parent->addChild(item);
        else tree_->addTopLevelItem(item);
        item->setText(0, q(slash == std::string::npos ? e.path : e.path.substr(slash + 1)));
        item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        item->setData(0, Qt::UserRole, q(e.path));
        item->setText(1, humanSize(e.size));
        item->setData(1, Qt::UserRole, qlonglong(e.size));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(2, QDateTime::fromSecsSinceEpoch(e.mtime).toString("yyyy-MM-dd HH:mm"));
        item->setData(2, Qt::UserRole, qlonglong(e.mtime));
        item->setText(3, QString("%1").arg(e.crc, 8, 16, QLatin1Char('0')));
        if (e.block == zzipp::NO_BLOCK) {
            item->setText(4, tr("empty"));
        } else {
            const zzipp::BlockInfo& b = idx.blocks[e.block];
            const double pct = 100.0 * double(b.comp_size) / double(b.raw_size);
            item->setText(4, QString("%1 · %2%").arg(methodName(b.method)).arg(pct, 0, 'f', 0));
            item->setToolTip(4, tr("Block %1: %2 → %3 on disk (%4% of original)\nFiles in the same block are compressed together.")
                                    .arg(e.block).arg(humanSize(b.raw_size), humanSize(b.comp_size)).arg(pct, 0, 'f', 1));
        }
    }

    // folder totals
    std::function<qlonglong(QTreeWidgetItem*)> total = [&](QTreeWidgetItem* it) -> qlonglong {
        auto* e = static_cast<EntryItem*>(it);
        if (!e->isDir) return it->data(1, Qt::UserRole).toLongLong();
        qlonglong s = 0;
        for (int i = 0; i < it->childCount(); ++i) s += total(it->child(i));
        it->setData(1, Qt::UserRole, s);
        it->setText(1, humanSize(uint64_t(s)));
        return s;
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) total(tree_->topLevelItem(i));
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    if (tree_->topLevelItemCount() == 1) tree_->topLevelItem(0)->setExpanded(true);
    applyFilter(filterEdit_->text());
}

void MainWindow::updateSummary() {
    if (!reader_) return;
    const auto& idx = reader_->index();
    uint64_t files = 0, dirs = 0, total = 0;
    for (const auto& e : idx.entries) {
        if (e.kind == zzipp::EntryKind::Dir) ++dirs;
        else ++files, total += e.size;
    }
    uint64_t dataBytes = 0;
    for (const auto& b : idx.blocks) dataBytes += b.comp_size;
    sumFiles_->setText(tr("%1 files, %2 folders").arg(files).arg(dirs));
    sumSize_->setText(humanSize(total));
    sumPacked_->setText(humanSize(reader_->file_size()));
    sumRatio_->setText(total ? tr("%1% of original").arg(100.0 * double(reader_->file_size()) / double(total), 0, 'f', 1) : tr("—"));
    (void)dataBytes;
}

void MainWindow::applyFilter(const QString& text) {
    const QString needle = text.trimmed();
    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* it) -> bool {
        bool any = false;
        for (int i = 0; i < it->childCount(); ++i) any |= visit(it->child(i));
        const bool self = needle.isEmpty() || it->data(0, Qt::UserRole).toString().contains(needle, Qt::CaseInsensitive);
        const bool show = self || any;
        it->setHidden(!show);
        if (any && !needle.isEmpty()) it->setExpanded(true);
        return show;
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) visit(tree_->topLevelItem(i));
}

QStringList MainWindow::selectedPaths() const {
    QStringList out;
    for (QTreeWidgetItem* it : tree_->selectedItems()) out << it->data(0, Qt::UserRole).toString();
    out.removeDuplicates();
    return out;
}

void MainWindow::updateBrowseButtons() {
    const bool has = reader_ != nullptr && !busy_;
    extractAllBtn_->setEnabled(has);
    testBtn_->setEnabled(has);
    zipBtn_->setEnabled(has);
    extractSelBtn_->setEnabled(has && !tree_->selectedItems().isEmpty());
}

int MainWindow::topLevelCount() const { return tree_->topLevelItemCount(); }

void MainWindow::startExtract(const QString& dest, const QStringList& selectors, bool overwrite) {
    if (busy_ || !reader_) return;
    zzipp::ExtractOptions o;
    o.archive = u8(archivePath_);
    o.out_dir = u8(dest);
    for (const QString& s : selectors) o.paths.push_back(u8(s));
    o.threads = unsigned(threadsSpin_->value());
    o.force = overwrite;
    runTask(selectors.isEmpty() ? tr("Extracting everything…") : tr("Extracting %1 item(s)…").arg(selectors.size()),
            [o](const zzipp::Callbacks& cb) mutable {
                o.cb = cb;
                return zzipp::cmd_extract(o);
            },
            [this, dest](bool ok, int rc, const QString& err) {
                if (!ok) {
                    if (!err.isEmpty()) notify(QMessageBox::Critical, tr("Extraction failed"), err);
                    return;
                }
                if (!interactive_) return;
                QMessageBox box(rc == 0 ? QMessageBox::Information : QMessageBox::Warning,
                                rc == 0 ? tr("Extraction complete") : tr("Finished with warnings"),
                                rc == 0 ? tr("Files were extracted to:\n%1").arg(dest)
                                        : tr("Some files were skipped (for example because they already exist). Open Details for the list."),
                                QMessageBox::NoButton, this);
                auto* open = box.addButton(tr("Open folder"), QMessageBox::AcceptRole);
                box.addButton(tr("Close"), QMessageBox::RejectRole);
                box.exec();
                if (box.clickedButton() == open) QDesktopServices::openUrl(QUrl::fromLocalFile(dest));
                if (rc != 0) detailsBtn_->setChecked(true);
            });
}

void MainWindow::startTest() {
    if (busy_ || !reader_) return;
    const std::string path = u8(archivePath_);
    const unsigned threads = unsigned(threadsSpin_->value());
    runTask(tr("Testing archive…"),
            [path, threads](const zzipp::Callbacks& cb) { return zzipp::cmd_test(path, threads, cb); },
            [this](bool ok, int, const QString& err) {
                if (ok) notify(QMessageBox::Information, tr("Archive is intact"), tr("Every block and file passed its checksum."));
                else if (!err.isEmpty()) notify(QMessageBox::Critical, tr("Integrity check failed"), err);
            });
}

void MainWindow::startExportZip(const QString& zipPath) {
    if (busy_ || !reader_) return;
    zzipp::ZipOptions o;
    o.archive = u8(archivePath_);
    o.zip_path = u8(zipPath);
    o.force = true;
    runTask(tr("Exporting ZIP…"),
            [o](const zzipp::Callbacks& cb) mutable {
                o.cb = cb;
                return zzipp::cmd_export_zip(o);
            },
            [this, zipPath](bool ok, int, const QString& err) {
                if (!ok) {
                    if (!err.isEmpty()) notify(QMessageBox::Critical, tr("ZIP export failed"), err);
                    return;
                }
                if (interactive_) QMessageBox::information(this, tr("ZIP exported"), tr("Saved %1").arg(QDir::toNativeSeparators(zipPath)));
            });
}

// ---------------------------------------------------------------------------- task runner ----

void MainWindow::runTask(const QString& title, std::function<int(const zzipp::Callbacks&)> work,
                         std::function<void(bool, int, const QString&)> done) {
    if (busy_) return;
    busy_ = true;
    cancel_ = false;
    progDone_ = 0;
    progTotal_ = 0;
    elapsed_.start();
    setBusyUi(true, title);
    appendLog(title);

    struct Shared {
        std::atomic<int> rc{0};
        QString error;
        bool cancelled = false, failed = false;
    };
    auto st = std::make_shared<Shared>();

    zzipp::Callbacks cb;
    cb.cancel = &cancel_;
    cb.progress = [this](uint64_t d, uint64_t t) {
        progDone_.store(d);
        progTotal_.store(t);
    };
    cb.log = [this](const std::string& m) {
        const QString s = q(m);
        QMetaObject::invokeMethod(this, [this, s] { appendLog(s); }, Qt::QueuedConnection);
    };

    QThread* th = QThread::create([work, cb, st] {
        try {
            st->rc = work(cb);
        } catch (const zzipp::Cancelled&) {
            st->cancelled = true;
        } catch (const std::exception& e) {
            st->failed = true;
            st->error = QString::fromUtf8(e.what());
        } catch (...) {
            st->failed = true;
            st->error = QStringLiteral("unknown error");
        }
    });
    worker_ = th;
    connect(th, &QThread::finished, this, [this, th, st, done] {
        th->deleteLater();
        worker_ = nullptr;
        busy_ = false;
        const bool ok = !st->failed && !st->cancelled;
        const qint64 ms = elapsed_.elapsed();
        setBusyUi(false);
        QString msg;
        if (st->cancelled) msg = tr("Cancelled");
        else if (st->failed) msg = tr("Failed: %1").arg(st->error);
        else msg = tr("Done in %1").arg(formatDuration(ms));
        statusLabel_->setText(msg);
        appendLog(msg);
        progress_->setRange(0, 1000);
        progress_->setValue(ok ? 1000 : 0);
        if (st->failed) detailsBtn_->setChecked(true);
        updateBrowseButtons();
        if (done) done(ok, st->rc.load(), st->error);
        emit taskFinished(ok, st->rc.load(), msg);
    });
    th->start();
}

void MainWindow::setBusyUi(bool busy, const QString& title) {
    stack_->setEnabled(!busy);
    sidebar_->setEnabled(!busy);
    cancelBtn_->setEnabled(busy);
    if (busy) {
        statusLabel_->setText(title);
        progress_->setRange(0, 0);
        timer_->start();
    } else {
        timer_->stop();
        elapsedLabel_->clear();
    }
}

void MainWindow::tick() {
    const uint64_t t = progTotal_.load(), d = progDone_.load();
    if (t == 0) {
        progress_->setRange(0, 0);  // busy indicator while the total is unknown
    } else {
        progress_->setRange(0, 1000);
        progress_->setValue(int(std::min<uint64_t>(1000, d * 1000 / t)));
    }
    QString e = formatDuration(elapsed_.elapsed());
    if (t) e = tr("%1% · %2").arg(int(std::min<uint64_t>(100, d * 100 / t))).arg(e);
    elapsedLabel_->setText(e);
}

void MainWindow::appendLog(const QString& line) { log_->appendPlainText(line); }

// ---------------------------------------------------------------------------- misc ----

void MainWindow::loadSettings() {
    QSettings s("zzipp", "zzipp");
    threadsSpin_->setValue(s.value("threads", int(zzipp::default_threads())).toInt());
    overwriteDefault_->setChecked(s.value("overwrite", false).toBool());
    overwriteCheck_->setChecked(overwriteDefault_->isChecked());
    lastDir_ = s.value("lastDir", QDir::homePath()).toString();
}

void MainWindow::saveSettings() {
    QSettings s("zzipp", "zzipp");
    s.setValue("threads", threadsSpin_->value());
    s.setValue("overwrite", overwriteDefault_->isChecked());
    s.setValue("lastDir", lastDir_);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (busy_) {
        if (interactive_ && QMessageBox::question(this, tr("Operation in progress"),
                                                  tr("Cancel the running operation and quit?")) != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        cancel_ = true;
        if (worker_) worker_->wait();
    }
    saveSettings();
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile()) paths << u.toLocalFile();
    if (paths.isEmpty() || busy_) return;
    if (paths.size() == 1 && paths.first().endsWith(".mzg", Qt::CaseInsensitive)) {
        openArchive(paths.first());
    } else {
        addInputs(paths);
        showPage(PageCreate);
    }
    e->acceptProposedAction();
}
