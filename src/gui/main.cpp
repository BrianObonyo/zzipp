#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <cstdio>
#include <random>

#include "MainWindow.hpp"

namespace {

QIcon makeIcon() {
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const double s = size / 256.0;
        QLinearGradient g(0, 0, size, size);
        g.setColorAt(0, QColor("#7B8CFF"));
        g.setColorAt(1, QColor("#3A4BD6"));
        p.setBrush(g);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(8 * s, 8 * s, 240 * s, 240 * s), 58 * s, 58 * s);
        QPen pen(Qt::white, 28 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath z;
        z.moveTo(84 * s, 84 * s);
        z.lineTo(172 * s, 84 * s);
        z.lineTo(84 * s, 172 * s);
        z.lineTo(172 * s, 172 * s);
        p.drawPath(z);
        icon.addPixmap(pm);
    }
    return icon;
}

void applyTheme(QApplication& app) {
    bool dark = app.palette().color(QPalette::Window).lightness() < 128;
    const QByteArray forced = qgetenv("ZZIPP_THEME");
    if (forced == "dark") dark = true;
    if (forced == "light") dark = false;

    struct C { QString bg, side, card, text, muted, border, hover, accent, accentHover, accentSoft, danger; };
    const C c = dark ? C{"#14161C", "#0F1115", "#1C1F27", "#E6E8EE", "#8E94A5", "#2C3040", "#232733", "#7B8CFF", "#92A0FF", "#2A3055", "#FF6B6B"}
                     : C{"#F4F5FA", "#FFFFFF", "#FFFFFF", "#1E2230", "#6B7285", "#E1E4EE", "#F0F2F9", "#4458E0", "#3548CC", "#E6EAFE", "#D64545"};

    app.setStyle("Fusion");
    QPalette pal;
    pal.setColor(QPalette::Window, c.bg);
    pal.setColor(QPalette::WindowText, c.text);
    pal.setColor(QPalette::Base, c.card);
    pal.setColor(QPalette::AlternateBase, c.hover);
    pal.setColor(QPalette::Text, c.text);
    pal.setColor(QPalette::Button, c.card);
    pal.setColor(QPalette::ButtonText, c.text);
    pal.setColor(QPalette::Highlight, c.accent);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::PlaceholderText, c.muted);
    pal.setColor(QPalette::ToolTipBase, c.card);
    pal.setColor(QPalette::ToolTipText, c.text);
    pal.setColor(QPalette::Disabled, QPalette::Text, c.muted);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, c.muted);
    app.setPalette(pal);

    QFont f = app.font();
    f.setPointSizeF(std::max(10.0, f.pointSizeF()));
    app.setFont(f);

    QString qss = R"(
QWidget#root { background: @bg@; }
QWidget#sidebar { background: @side@; border-right: 1px solid @border@; }
QLabel { color: @text@; background: transparent; }
QLabel#appName { font-size: 17pt; font-weight: 700; }
QLabel#h1 { font-size: 21pt; font-weight: 700; }
QLabel#sub, QLabel#muted { color: @muted@; }
QLabel#cardTitle { font-weight: 700; font-size: 11pt; }
QLabel#fieldLabel { font-weight: 600; }
QLabel#stat { font-size: 13pt; font-weight: 700; }
QLabel#status { font-weight: 600; }
QFrame#card { background: @card@; border: 1px solid @border@; border-radius: 12px; }
QFrame#card QLabel { background: transparent; }
QPushButton#nav { text-align: left; padding: 10px 14px; border: none; border-radius: 9px; color: @muted@; background: transparent; font-weight: 600; }
QPushButton#nav:hover { background: @hover@; color: @text@; }
QPushButton#nav:checked { background: @accentSoft@; color: @accent@; }
QPushButton { background: @card@; border: 1px solid @border@; border-radius: 9px; padding: 7px 15px; color: @text@; }
QPushButton:hover { border-color: @accent@; }
QPushButton:checked { background: @accentSoft@; border-color: @accent@; }
QPushButton:disabled { color: @muted@; background: transparent; }
QPushButton#primary { background: @accent@; color: @onAccent@; border: 1px solid @accent@; font-weight: 600; padding: 9px 22px; }
QPushButton#primary:hover { background: @accentHover@; }
QPushButton#primary:disabled { background: @border@; border-color: @border@; color: @muted@; }
QPushButton#danger { color: @danger@; }
QPushButton#danger:hover { border-color: @danger@; }
QPushButton#danger:disabled { color: @muted@; }
QLineEdit, QComboBox, QSpinBox { background: @card@; border: 1px solid @border@; border-radius: 9px; padding: 6px 10px; selection-background-color: @accent@; color: @text@; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: @accent@; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox QAbstractItemView { background: @card@; border: 1px solid @border@; selection-background-color: @accentSoft@; selection-color: @text@; outline: 0; }
QListWidget, QTreeWidget { background: @card@; border: 1px solid @border@; border-radius: 10px; outline: 0; color: @text@; }
QListWidget::item { padding: 6px 8px; }
QTreeWidget::item { padding: 3px 4px; }
QListWidget::item:selected, QTreeWidget::item:selected { background: @accentSoft@; color: @text@; }
QTreeWidget::item:hover, QListWidget::item:hover { background: @hover@; }
QHeaderView::section { background: @card@; border: none; border-bottom: 1px solid @border@; padding: 7px 8px; color: @muted@; font-weight: 600; }
QProgressBar { background: @border@; border: none; border-radius: 5px; }
QProgressBar::chunk { background: @accent@; border-radius: 5px; }
QPlainTextEdit { background: @side@; border: 1px solid @border@; border-radius: 8px; font-family: monospace; color: @text@; }
QCheckBox { color: @text@; spacing: 8px; }
QScrollBar:vertical { background: transparent; width: 12px; margin: 2px; }
QScrollBar::handle:vertical { background: @border@; border-radius: 4px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: @muted@; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QToolTip { background: @card@; color: @text@; border: 1px solid @border@; padding: 4px; }
)";
    const QPair<QString, QString> map[] = {{"@onAccent@", dark ? QString("#0E1020") : QString("#FFFFFF")}, {"@bg@", c.bg}, {"@side@", c.side}, {"@card@", c.card}, {"@text@", c.text},
                                           {"@muted@", c.muted}, {"@border@", c.border}, {"@hover@", c.hover},
                                           {"@accentHover@", c.accentHover}, {"@accentSoft@", c.accentSoft},
                                           {"@accent@", c.accent}, {"@danger@", c.danger}};
    for (const auto& kv : map) qss.replace(kv.first, kv.second);
    app.setStyleSheet(qss);
}

// ------------------------------------------------------------------ headless self-test ----

bool writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}
QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

int runSelfTest(MainWindow& w, const QString& work, const QString& shots) {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++failures;
    };
    auto waitTask = [&](const std::function<void()>& start, bool* okOut = nullptr, QString* msgOut = nullptr, int* rcOut = nullptr) {
        QEventLoop loop;
        bool finished = false, ok = false;
        int rc = 0;
        QString msg;
        QObject::connect(&w, &MainWindow::taskFinished, &loop, [&](bool o, int r, const QString& m) {
            finished = true; ok = o; rc = r; msg = m; loop.quit();
        });
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        start();
        if (!w.busy() && !finished) return false;  // task refused to start
        if (!finished) loop.exec();
        if (okOut) *okOut = ok;
        if (msgOut) *msgOut = msg;
        if (rcOut) *rcOut = rc;
        return finished;
    };

    QDir(work).removeRecursively();
    const QString src = work + "/src";
    std::mt19937 rng(42);
    QByteArray text, noise;
    for (int i = 0; i < 4000; ++i) text += "line " + QByteArray::number(i % 37) + " of repeated sample text for solid compression\n";
    for (int i = 0; i < 100000; ++i) noise.append(char(rng()));
    writeFile(src + "/docs/a.txt", text);
    writeFile(src + "/docs/b copy.txt", text);  // duplicate
    writeFile(src + "/data/random.bin", noise);
    writeFile(src + "/ünïcode/файл.txt", "unicode names\n");
    writeFile(src + "/empty.txt", "");
    QDir().mkpath(src + "/empty_dir");

    w.setInteractive(false);
    w.resize(1040, 720);
    w.show();
    w.addInputs({src});
    w.setOutputPath(work + "/out.mzg");
    w.setPreset(1);
    if (!shots.isEmpty()) {
        QDir().mkpath(shots);
        w.showPage(MainWindow::PageCreate);
        QApplication::processEvents();
        w.grab().save(shots + "/create.png");
    }

    bool ok = false;
    QString msg;
    check(waitTask([&] { w.startCreate(); }, &ok, &msg) && ok && QFileInfo::exists(work + "/out.mzg"), "create archive through the GUI");
    check(!QFileInfo::exists(work + "/out.mzg.partial"), "no .partial file left behind");

    w.openArchive(work + "/out.mzg");
    check(w.topLevelCount() == 1, "open archive and populate tree");
    if (!shots.isEmpty()) {
        QApplication::processEvents();
        w.grab().save(shots + "/browse.png");
    }

    check(waitTask([&] { w.startExtract(work + "/restored", {}, false); }, &ok) && ok, "extract everything");
    for (const char* rel : {"docs/a.txt", "docs/b copy.txt", "data/random.bin"})
        check(readFile(work + "/src/" + rel) == readFile(work + "/restored/src/" + rel) && QFileInfo::exists(work + "/restored/src/" + rel),
              rel);
    check(readFile(work + "/restored/src/ünïcode/файл.txt") == "unicode names\n", "unicode file names round-trip");
    check(QFileInfo(work + "/restored/src/empty_dir").isDir(), "empty directory restored");
    check(QFileInfo(work + "/restored/src/empty.txt").exists() && QFileInfo(work + "/restored/src/empty.txt").size() == 0, "empty file restored");

    check(waitTask([&] { w.startExtract(work + "/partial", {"src/docs/a.txt"}, false); }, &ok) && ok &&
              QFileInfo::exists(work + "/partial/src/docs/a.txt") && !QFileInfo::exists(work + "/partial/src/data/random.bin"),
          "extract a single selected file");

    check(waitTask([&] { w.startTest(); }, &ok) && ok, "test integrity");
    check(waitTask([&] { w.startExportZip(work + "/out.zip"); }, &ok) && ok && QFileInfo(work + "/out.zip").size() > 0, "export ZIP");
    if (!QStandardPaths::findExecutable("unzip").isEmpty()) {
        QProcess p;
        p.start("unzip", {"-tq", work + "/out.zip"});
        p.waitForFinished(30000);
        check(p.exitCode() == 0, "unzip accepts the exported ZIP");
    }

    // ---- error paths: nothing may crash, state must stay sane, messages must be useful ----
    {
        const int before = w.topLevelCount();
        writeFile(work + "/garbage.mzg", QByteArray(500, 'x'));
        w.openArchive(work + "/garbage.mzg");
        check(w.topLevelCount() == before && before > 0, "opening a non-archive keeps the current archive open");

        w.clearInputs();
        w.addInputs({work + "/does/not/exist"});
        w.setOutputPath(work + "/never.mzg");
        bool o2 = true;
        QString m2;
        check(waitTask([&] { w.startCreate(); }, &o2, &m2) && !o2 && m2.contains("cannot access"), "create with a missing input fails with 'cannot access'");
        check(!QFileInfo::exists(work + "/never.mzg") && !QFileInfo::exists(work + "/never.mzg.partial"), "failed create leaves no files");

        w.clearInputs();
        check(!waitTask([&] { w.startCreate(); }), "create with no inputs does not start a task");

        writeFile(work + "/afile", "x");
        int rc = 0;
        const bool fin = waitTask([&] { w.startExtract(work + "/afile", {}, false); }, &o2, &m2, &rc);
        check(fin && ((!o2 && m2.contains("cannot create")) || (o2 && rc != 0)), "extracting into a path that is a file is reported, not ignored");

        check(waitTask([&] { w.startExtract(work + "/restored", {}, false); }, &o2, &m2, &rc) && o2 && rc != 0, "re-extract without overwrite reports skipped files");
        check(waitTask([&] { w.startExtract(work + "/restored", {}, true); }, &o2, &m2, &rc) && o2 && rc == 0, "re-extract with overwrite succeeds");
    }

    // Cancel a long-running create: no archive and no partial file may remain.
    {
        QDir().mkpath(work + "/big");
        QFile f(work + "/big/big.txt");
        f.open(QIODevice::WriteOnly);
        for (int i = 0; i < 100; ++i) {
            QByteArray chunk;
            for (int j = 0; j < 20000; ++j) chunk += "w" + QByteArray::number(int(rng() % 90000)) + (j % 12 == 0 ? "\n" : " ");
            f.write(chunk);
        }
        f.close();
        w.clearInputs();
        w.addInputs({work + "/big"});
        w.setOutputPath(work + "/cancelled.mzg");
        w.setPreset(0);
        QTimer::singleShot(400, &w, [&] { w.requestCancel(); });
        const bool finished = waitTask([&] { w.startCreate(); }, &ok, &msg);
        check(finished && !ok && msg == "Cancelled", "cancel a running create");
        check(!QFileInfo::exists(work + "/cancelled.mzg") && !QFileInfo::exists(work + "/cancelled.mzg.partial"), "cancelled create leaves no files");
    }

    if (!shots.isEmpty()) {
        w.showPage(MainWindow::PageSettings);
        QApplication::processEvents();
        w.grab().save(shots + "/settings.png");
    }
    std::printf("%s\n", failures ? "SELFTEST FAILED" : "SELFTEST OK");
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("zzipp");
    app.setOrganizationName("zzipp");
    app.setWindowIcon(makeIcon());
    applyTheme(app);

    const QStringList args = app.arguments();
    QString selftest, shots, open;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--selftest" && i + 1 < args.size()) selftest = args[++i];
        else if (args[i] == "--screenshots" && i + 1 < args.size()) shots = args[++i];
        else if (!args[i].startsWith("--")) open = args[i];
    }

    MainWindow w;
    if (!selftest.isEmpty()) return runSelfTest(w, selftest, shots);
    w.show();
    if (!open.isEmpty()) w.openArchive(open);
    return app.exec();
}
