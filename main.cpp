// omasettings - click-through settings for an Omarchy desktop.
// Every page writes a plain config file under ~/.config, so anything set here
// can also be edited by hand, and hand edits show up here.
//
// Pages so far: Mouse pointer, Windows, Keyboard & touchpad, Display.
//
//   omasettings                        open the window
//   omasettings --page windows         open on a page
//   omasettings pointer                print the current pointer and size
//   omasettings pointer <theme> [size] set it without opening the window

#include <QApplication>
#include <QWidget>
#include <QAbstractButton>
#include <QPushButton>
#include <QButtonGroup>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QStyleOptionSlider>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QComboBox>
#include <functional>
#include <QSlider>
#include <QSignalBlocker>
#include <QListWidget>
#include <QStackedWidget>
#include <QScrollArea>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHash>
#include <QSet>
#include <QFont>
#include <QFontInfo>
#include <QFontDatabase>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QStyle>
#include <QStyleOption>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QTimer>
#include <QtEndian>
#include <QtMath>
#include <QTextStream>

// ---------------------------------------------------------------- shared look

static QString uiFontFamily()
{
    const QString sys = QFontInfo(QFont(QStringLiteral("monospace"))).family();
    return sys.isEmpty() ? QStringLiteral("monospace") : sys;
}

// Surfaces are ink mixed into the page rather than hardcoded greys, the way
// omacalc and omatimer do it, so every theme looks right.
static QColor mix(const QColor &base, const QColor &tint, qreal amount)
{
    return QColor::fromRgbF(base.redF() + (tint.redF() - base.redF()) * amount,
                            base.greenF() + (tint.greenF() - base.greenF()) * amount,
                            base.blueF() + (tint.blueF() - base.blueF()) * amount);
}

struct Palette {
    QColor page = QColor("#1a1a1a"), ink = QColor("#cccccc"), accent = QColor("#7aa2f7");
};

static QString omarchyColorsPath()
{
    return QDir::homePath() + "/.local/state/omarchy/current/theme/colors.toml";
}

// Same file omacalc, omawrite and omatimer read. A light theme's mode flips
// the pair so the page stays in the theme's palette rather than flat white.
static Palette loadOmarchyPalette()
{
    Palette p;
    QFile f(omarchyColorsPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return p;
    static const QRegularExpression entry("^\\s*(\\w+)\\s*=\\s*\"([^\"]+)\"");
    QHash<QString, QString> c;
    while (!f.atEnd()) {
        const auto m = entry.match(QString::fromUtf8(f.readLine()));
        if (m.hasMatch())
            c.insert(m.captured(1), m.captured(2));
    }
    if (c.contains("background")) {
        p.page = QColor(c.value("background"));
        p.ink = QColor(c.value("foreground", p.ink.name()));
        p.accent = QColor(c.value("accent", p.accent.name()));
    }
    const bool light = c.value("mode").compare("light", Qt::CaseInsensitive) == 0;
    if (light != (p.page.lightnessF() > p.ink.lightnessF()))
        qSwap(p.page, p.ink);
    return p;
}

// ------------------------------------------------------------ xcursor files

// Cursor themes are folders of Xcursor files: a small table of contents, then
// one ARGB image per nominal size (and per frame, for animated ones). Qt can't
// read them, so this pulls out the first frame nearest the size asked for.
struct CursorImage {
    QImage image;
    QPoint hotspot;
    bool isNull() const { return image.isNull(); }
};

static CursorImage readXcursor(const QString &path, int size)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray d = f.readAll();
    auto u32 = [&d](qint64 off) -> quint32 {
        if (off < 0 || off + 4 > d.size())
            return 0;
        return qFromLittleEndian<quint32>(d.constData() + off);
    };
    if (d.size() < 16 || !d.startsWith("Xcur"))
        return {};
    static const quint32 kImageType = 0xfffd0002;
    const quint32 header = u32(4), entries = qMin<quint32>(u32(12), 4096);

    // Smallest image at or above the size scales down cleanly; below it only
    // as a last resort, since scaling up blurs.
    qint64 bestPos = -1;
    quint32 bestSize = 0;
    qint64 bestScore = LLONG_MAX;
    for (quint32 i = 0; i < entries; ++i) {
        const qint64 off = header + qint64(i) * 12;
        if (u32(off) != kImageType)
            continue;
        const quint32 nominal = u32(off + 4);
        const qint64 score = nominal >= quint32(size) ? nominal - size : (size - nominal) * 4;
        if (score < bestScore) {
            bestScore = score;
            bestSize = nominal;
            bestPos = u32(off + 8);
        }
    }
    if (bestPos < 0 || bestSize == 0)
        return {};

    const quint32 w = u32(bestPos + 16), h = u32(bestPos + 20);
    const quint32 xhot = u32(bestPos + 24), yhot = u32(bestPos + 28);
    const qint64 pixels = bestPos + 36;
    if (w == 0 || h == 0 || w > 1024 || h > 1024 || pixels + qint64(w) * h * 4 > d.size())
        return {};

    QImage img(int(w), int(h), QImage::Format_ARGB32_Premultiplied);
    for (quint32 y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<quint32 *>(img.scanLine(int(y)));
        for (quint32 x = 0; x < w; ++x)
            line[x] = u32(pixels + (qint64(y) * w + x) * 4);
    }

    CursorImage out{img, QPoint(int(xhot), int(yhot))};
    if (bestSize != quint32(size)) {
        const qreal k = qreal(size) / bestSize;
        out.image = img.scaled(qMax(1, qRound(w * k)), qMax(1, qRound(h * k)),
                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        out.hotspot = QPoint(qRound(xhot * k), qRound(yhot * k));
    }
    return out;
}

// The four shapes a card shows. Themes name them differently, so each has a
// few spellings to try.
static const QList<QStringList> kPreviewShapes = {
    {"left_ptr", "default", "arrow", "top_left_arrow"},
    {"pointer", "hand2", "hand1", "pointing_hand"},
    {"text", "xterm", "ibeam"},
    {"wait", "watch", "progress", "left_ptr_watch"},
};

struct CursorTheme {
    QString id;    // folder name, what XCURSOR_THEME wants
    QString name;  // index.theme's Name=, for display
    QString dir;   // .../<id>/cursors
    QStringList inherits; // parents to borrow missing shapes from
};

// Same search order as libXcursor, so a user-installed theme shadows a
// system one of the same name.
static QStringList iconDirs()
{
    const QString home = QDir::homePath();
    return {home + "/.local/share/icons", home + "/.icons", "/usr/local/share/icons",
            "/usr/share/icons"};
}

static QList<CursorTheme> findCursorThemes()
{
    QList<CursorTheme> themes;
    QSet<QString> seen;
    for (const QString &base : iconDirs()) {
        const QDir root(base);
        for (const QString &id : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString cursors = root.filePath(id) + "/cursors";
            if (seen.contains(id) || !QFileInfo(cursors).isDir())
                continue;
            seen.insert(id);
            QString name;
            QStringList inherits;
            QFile index(root.filePath(id) + "/index.theme");
            if (index.open(QIODevice::ReadOnly | QIODevice::Text)) {
                while (!index.atEnd()) {
                    const QString line = QString::fromUtf8(index.readLine()).trimmed();
                    if (line.startsWith("Name=") && name.isEmpty())
                        name = line.mid(5).trimmed();
                    else if (line.startsWith("Inherits="))
                        inherits = line.mid(9).split(',', Qt::SkipEmptyParts);
                }
            }
            // "Bibata-Modern-Ice" reads better as "Bibata Modern Ice".
            if (name.isEmpty() || name == id)
                name = QString(id).replace('-', ' ').replace('_', ' ');
            for (QString &i : inherits)
                i = i.trimmed();
            themes.append({id, name, cursors, inherits});
        }
    }
    std::sort(themes.begin(), themes.end(), [](const CursorTheme &a, const CursorTheme &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return themes;
}

static QString findThemeDir(const QString &id)
{
    for (const QString &base : iconDirs()) {
        const QString dir = base + "/" + id + "/cursors";
        if (QFileInfo(dir).isDir())
            return dir;
    }
    return QString();
}

static QStringList themeInherits(const QString &id)
{
    for (const QString &base : iconDirs()) {
        QFile index(base + "/" + id + "/index.theme");
        if (!index.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        while (!index.atEnd()) {
            const QString line = QString::fromUtf8(index.readLine()).trimmed();
            if (line.startsWith("Inherits=")) {
                QStringList out = line.mid(9).split(',', Qt::SkipEmptyParts);
                for (QString &i : out)
                    i = i.trimmed();
                return out;
            }
        }
        return {};
    }
    return {};
}

// A theme can leave shapes out and name a parent to fill them in, the way
// the left-handed Phinger sets borrow the text and wait shapes.
static CursorImage loadShape(const CursorTheme &t, const QStringList &names, int size)
{
    QStringList dirs{t.dir};
    QStringList queue = t.inherits;
    QSet<QString> visited{t.id};
    while (!queue.isEmpty() && visited.size() < 8) {
        const QString id = queue.takeFirst();
        if (visited.contains(id))
            continue;
        visited.insert(id);
        const QString dir = findThemeDir(id);
        if (!dir.isEmpty())
            dirs.append(dir);
        queue.append(themeInherits(id));
    }
    for (const QString &dir : dirs) {
        for (const QString &n : names) {
            const QString path = dir + "/" + n;
            if (QFileInfo::exists(path)) {
                CursorImage img = readXcursor(path, size);
                if (!img.isNull())
                    return img;
            }
        }
    }
    return {};
}

// ------------------------------------------------------------ config file

// Pointer settings live in their own file, required from hyprland.lua, so the
// app can rewrite it whole without touching anything hand-written.
static QString cursorConfigPath()
{
    return QDir::homePath() + "/.config/hypr/cursor.lua";
}

struct CursorSetting {
    QString theme = QStringLiteral("Adwaita");
    int size = 24;
};

static CursorSetting readCursorSetting()
{
    CursorSetting s;
    QFile f(cursorConfigPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return s;
    static const QRegularExpression env(
        "hl\\.env\\(\\s*\"XCURSOR_(THEME|SIZE)\"\\s*,\\s*\"([^\"]*)\"\\s*\\)");
    auto it = env.globalMatch(QString::fromUtf8(f.readAll()));
    while (it.hasNext()) {
        const auto m = it.next();
        if (m.captured(1) == "THEME")
            s.theme = m.captured(2);
        else if (m.captured(2).toInt() > 0)
            s.size = m.captured(2).toInt();
    }
    return s;
}

static bool writeCursorSetting(const CursorSetting &s)
{
    QSaveFile f(cursorConfigPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    const QString size = QString::number(s.size);
    f.write(QString("-- Mouse pointer. Written by omasettings, which rewrites this whole\n"
                    "-- file when you pick a pointer there; editing it by hand works too.\n"
                    "-- Themes are folders in ~/.local/share/icons or /usr/share/icons.\n"
                    "hl.env(\"XCURSOR_THEME\", \"%1\")\n"
                    "hl.env(\"XCURSOR_SIZE\", \"%2\")\n"
                    "hl.env(\"HYPRCURSOR_THEME\", \"%1\")\n"
                    "hl.env(\"HYPRCURSOR_SIZE\", \"%2\")\n")
                .arg(s.theme, size)
                .toUtf8());
    return f.commit();
}

// cursor.lua only counts if hyprland.lua loads it. A fresh Omarchy's
// doesn't, so the first save adds the line, or the pick would be gone at the
// next login.
static void ensureCursorRequired()
{
    const QString path = QDir::homePath() + "/.config/hypr/hyprland.lua";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    const QString text = QString::fromUtf8(f.readAll());
    f.close();
    static const QRegularExpression loaded("^\\s*require\\s*\\(?\\s*[\"']hypr\\.cursor[\"']",
                                           QRegularExpression::MultilineOption);
    if (loaded.match(text).hasMatch())
        return;
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write(QString("%1\n-- Mouse pointer theme and size. Set from the omasettings app, or edit\n"
                        "-- hypr/cursor.lua directly.\nrequire(\"hypr.cursor\")\n")
                    .arg(text.endsWith('\n') ? "" : "\n")
                    .toUtf8());
}

// Pointer styles that can be fetched into ~/.local/share/icons, no password
// needed. A download can hold several (Phinger's has four).
struct CursorDownload {
    QString name;
    QStringList ids; // folders it unpacks, to tell when it's installed
    QString url;
};

static const QList<CursorDownload> &cursorDownloads()
{
    static const QString bibata = "https://github.com/ful1e5/Bibata_Cursor/releases/download/v2.0.7/";
    static const QString catppuccin = "https://github.com/catppuccin/cursors/releases/download/v2.0.0/";
    static const QString google = "https://github.com/ful1e5/Google_Cursor/releases/download/v2.0.0/";
    static const QString apple = "https://github.com/ful1e5/apple_cursor/releases/download/v2.0.1/";
    static const QList<CursorDownload> list = {
        {"Bibata Modern Ice", {"Bibata-Modern-Ice"}, bibata + "Bibata-Modern-Ice.tar.xz"},
        {"Bibata Modern Classic", {"Bibata-Modern-Classic"}, bibata + "Bibata-Modern-Classic.tar.xz"},
        {"Bibata Modern Amber", {"Bibata-Modern-Amber"}, bibata + "Bibata-Modern-Amber.tar.xz"},
        {"Catppuccin Mocha Dark", {"catppuccin-mocha-dark-cursors"}, catppuccin + "catppuccin-mocha-dark-cursors.zip"},
        {"Catppuccin Mocha Light", {"catppuccin-mocha-light-cursors"}, catppuccin + "catppuccin-mocha-light-cursors.zip"},
        {"Catppuccin Mocha Mauve", {"catppuccin-mocha-mauve-cursors"}, catppuccin + "catppuccin-mocha-mauve-cursors.zip"},
        {"Catppuccin Latte Light", {"catppuccin-latte-light-cursors"}, catppuccin + "catppuccin-latte-light-cursors.zip"},
        {"GoogleDot Black", {"GoogleDot-Black"}, google + "GoogleDot-Black.tar.gz"},
        {"GoogleDot White", {"GoogleDot-White"}, google + "GoogleDot-White.tar.gz"},
        {"GoogleDot Blue", {"GoogleDot-Blue"}, google + "GoogleDot-Blue.tar.gz"},
        {"macOS", {"macOS"}, apple + "macOS.tar.xz"},
        {"macOS White", {"macOS-White"}, apple + "macOS-White.tar.xz"},
        {"Phinger (dark, light, and left-handed)",
         {"phinger-cursors-dark", "phinger-cursors-light", "phinger-cursors-dark-left", "phinger-cursors-light-left"},
         "https://github.com/phisch/phinger-cursors/releases/download/v2.1/phinger-cursors-variants.tar.bz2"},
    };
    return list;
}

// curl and bsdtar both come with every Arch install (pacman needs them), and
// bsdtar unpacks zip and every tar flavour alike. Any folder in the archive
// with a cursors/ inside is a theme and moves into place.
static const char *kInstallCursorScript = R"sh(
set -e
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL "$1" -o "$tmp/archive"
mkdir "$tmp/x"
bsdtar -xf "$tmp/archive" -C "$tmp/x"
mkdir -p "$2"
find "$tmp/x" -mindepth 2 -maxdepth 4 -type d -name cursors | while read -r c; do
  theme=$(dirname "$c")
  rm -rf "$2/$(basename "$theme")"
  mv "$theme" "$2/"
done
)sh";

// The file sets it for apps launched from now on; these switch the pointer
// that's on screen and tell GTK apps, which read gsettings instead.
static void applyCursorLive(const CursorSetting &s)
{
    const QString size = QString::number(s.size);
    QProcess::startDetached("hyprctl", {"setcursor", s.theme, size});
    QProcess::startDetached("gsettings", {"set", "org.gnome.desktop.interface", "cursor-theme", s.theme});
    QProcess::startDetached("gsettings", {"set", "org.gnome.desktop.interface", "cursor-size", size});
}

// ------------------------------------------------------------ pointer page

class ThemeCard : public QAbstractButton
{
public:
    ThemeCard(const CursorTheme &t, QWidget *parent) : QAbstractButton(parent), theme(t)
    {
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setToolTip(t.id);
    }

    const CursorTheme theme;

    void setColors(const Palette &p)
    {
        pal = p;
        update();
    }

    // Loaded at device pixels so they're crisp on a scaled screen, and
    // drawn at the logical size the pointer will really have.
    void setPointerSize(int size)
    {
        pointerSize = size;
        const qreal dpr = devicePixelRatioF();
        shapes.clear();
        for (const QStringList &names : kPreviewShapes) {
            CursorImage img = loadShape(theme, names, qRound(size * dpr));
            img.image.setDevicePixelRatio(dpr);
            shapes.append(img);
        }
        // Hovering the card shows its real arrow as the pointer: a try-on.
        if (!shapes.isEmpty() && !shapes[0].isNull()) {
            const QPixmap pm = QPixmap::fromImage(shapes[0].image);
            setCursor(QCursor(pm, qRound(shapes[0].hotspot.x() / dpr),
                              qRound(shapes[0].hotspot.y() / dpr)));
        } else {
            unsetCursor();
        }
        setFixedSize(sizeHint());
        update();
    }

    QSize sizeHint() const override
    {
        const int cell = qMax(pointerSize, 24) + 16;
        return QSize(qMax(4 * cell + 32, 240), cell + 56);
    }

protected:
    void enterEvent(QEnterEvent *e) override
    {
        hovered = true;
        update();
        QAbstractButton::enterEvent(e);
    }
    void leaveEvent(QEvent *e) override
    {
        hovered = false;
        update();
        QAbstractButton::leaveEvent(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
        const QColor fill = mix(pal.page, pal.ink, hovered ? 0.10 : 0.05);
        const QColor edge = isChecked() ? pal.accent
                            : hasFocus() ? mix(pal.page, pal.ink, 0.45)
                                         : mix(pal.page, pal.ink, 0.12);
        p.setPen(QPen(edge, isChecked() ? 3 : 1.5));
        p.setBrush(fill);
        p.drawRoundedRect(r, 8, 8);

        // Shapes sit centered in equal cells above the name.
        const int cell = qMax(pointerSize, 24) + 16;
        const int total = cell * shapes.size();
        int x = (width() - total) / 2;
        const int top = 12;
        for (const CursorImage &s : shapes) {
            if (!s.isNull()) {
                const QSizeF logical = s.image.deviceIndependentSize();
                p.drawImage(QPointF(x + (cell - logical.width()) / 2,
                                    top + (cell - logical.height()) / 2),
                            s.image);
            } else {
                p.setPen(mix(pal.page, pal.ink, 0.3));
                p.drawText(QRect(x, top, cell, cell), Qt::AlignCenter, "–");
            }
            x += cell;
        }

        QFont f(uiFontFamily());
        f.setPointSizeF(10);
        f.setBold(isChecked());
        p.setFont(f);
        p.setPen(isChecked() ? pal.accent : pal.ink);
        const QRect label(12, top + cell + 8, width() - 24, 24);
        p.drawText(label, Qt::AlignCenter,
                   QFontMetrics(f).elidedText(theme.name, Qt::ElideRight, label.width()));
    }

private:
    Palette pal;
    QList<CursorImage> shapes;
    int pointerSize = 24;
    bool hovered = false;
};

class PointerPage : public QWidget
{
public:
    explicit PointerPage(QWidget *parent = nullptr) : QWidget(parent)
    {
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(28, 24, 28, 20);
        outer->setSpacing(14);

        title = new QLabel("Mouse pointer");
        title->setObjectName("title");
        hint = new QLabel("Click a style to use it. Hover over one to try it on.");
        hint->setObjectName("quiet");
        hint->setWordWrap(true);
        outer->addWidget(title);
        outer->addWidget(hint);

        auto *sizeRow = new QHBoxLayout;
        sizeRow->setSpacing(6);
        auto *sizeLabel = new QLabel("Size");
        sizeLabel->setMinimumWidth(48);
        sizeRow->addWidget(sizeLabel);
        sizeGroup = new QButtonGroup(this);
        for (int s : {24, 32, 40, 48, 64}) {
            auto *b = new QPushButton(QString::number(s));
            b->setCheckable(true);
            b->setCursor(Qt::PointingHandCursor);
            b->setObjectName("chip");
            sizeGroup->addButton(b, s);
            sizeRow->addWidget(b);
        }
        sizeRow->addStretch();
        auto *more = new QPushButton("Get more styles");
        more->setObjectName("chip");
        more->setCursor(Qt::PointingHandCursor);
        sizeRow->addWidget(more);
        connect(more, &QPushButton::clicked, this, [this] { showDownloads(); });
        outer->addLayout(sizeRow);

        grid = new QWidget;
        gridLayout = new QGridLayout(grid);
        gridLayout->setContentsMargins(0, 0, 0, 0);
        gridLayout->setSpacing(12);
        gridLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        scroll = new QScrollArea;
        scroll->setWidget(grid);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        outer->addWidget(scroll, 1);
        scroll->viewport()->installEventFilter(this);

        auto *footer = new QHBoxLayout;
        fileLabel = new QLabel;
        fileLabel->setObjectName("quiet");
        fileLabel->setWordWrap(true);
        auto *openFile = new QPushButton("Open file");
        openFile->setObjectName("chip");
        openFile->setCursor(Qt::PointingHandCursor);
        footer->addWidget(fileLabel, 1);
        footer->addWidget(openFile);
        outer->addLayout(footer);

        cardGroup = new QButtonGroup(this);
        cardGroup->setExclusive(true);

        connect(sizeGroup, &QButtonGroup::idClicked, this, [this](int size) {
            current.size = size;
            save();
            setCardSizes();
        });
        connect(cardGroup, &QButtonGroup::buttonClicked, this, [this](QAbstractButton *b) {
            current.theme = static_cast<ThemeCard *>(b)->theme.id;
            save();
        });
        connect(openFile, &QPushButton::clicked, this, [] {
            if (!QProcess::startDetached("omarchy-launch-editor", {cursorConfigPath()}))
                QProcess::startDetached("xdg-open", {cursorConfigPath()});
        });

        // Hand edits to the file show up here. Saves replace the file rather
        // than edit it, so the path has to be re-added each time.
        watcher = new QFileSystemWatcher(this);
        watcher->addPath(QFileInfo(cursorConfigPath()).absolutePath());
        watcher->addPath(cursorConfigPath());
        connect(watcher, &QFileSystemWatcher::fileChanged, this, [this] { reloadFromFile(); });
        connect(watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
            if (!watcher->files().contains(cursorConfigPath()) && QFileInfo::exists(cursorConfigPath()))
                watcher->addPath(cursorConfigPath());
        });

        buildCards();
        reloadFromFile();
    }

    void setColors(const Palette &p)
    {
        pal = p;
        for (ThemeCard *c : cards)
            c->setColors(p);
    }

protected:
    // The viewport, not this page, is what the cards have to fit: it's
    // resized after the page, and loses width to the scrollbar.
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == scroll->viewport() && event->type() == QEvent::Resize)
            relayout();
        return QWidget::eventFilter(watched, event);
    }

private:
    void buildCards()
    {
        for (const CursorTheme &t : findCursorThemes()) {
            auto *card = new ThemeCard(t, grid);
            card->setColors(pal);
            cardGroup->addButton(card);
            cards.append(card);
        }
    }

    void reloadFromFile()
    {
        const CursorSetting s = readCursorSetting();
        const bool resized = s.size != current.size;
        current = s;
        if (QAbstractButton *b = sizeGroup->button(s.size))
            b->setChecked(true);
        else if (sizeGroup->checkedButton()) {
            // A size typed into the file that isn't one of the chips.
            sizeGroup->setExclusive(false);
            sizeGroup->checkedButton()->setChecked(false);
            sizeGroup->setExclusive(true);
        }
        for (ThemeCard *c : cards)
            c->setChecked(c->theme.id == s.theme);
        if (resized || firstLoad)
            setCardSizes();
        firstLoad = false;
        fileLabel->setText(QString("Saved to %1  ·  %2 at %3 px")
                               .arg(QString(cursorConfigPath()).replace(QDir::homePath(), "~"),
                                    s.theme)
                               .arg(s.size));
    }

    void setCardSizes()
    {
        for (ThemeCard *c : cards)
            c->setPointerSize(current.size);
        relayout();
    }

    void relayout()
    {
        if (cards.isEmpty())
            return;
        const int cardW = cards.first()->width();
        const int avail = scroll->viewport()->width();
        const int cols = qMax(1, (avail + gridLayout->spacing()) / (cardW + gridLayout->spacing()));
        if (cols == columns && gridLayout->count() == cards.size())
            return;
        columns = cols;
        grid->setMinimumWidth(0);
        for (ThemeCard *c : cards)
            gridLayout->removeWidget(c);
        for (int i = 0; i < cards.size(); ++i)
            gridLayout->addWidget(cards[i], i / cols, i % cols);
    }

    void showDownloads()
    {
        auto *dialog = new QWidget(window(), Qt::Dialog);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName("root");
        dialog->setWindowTitle("More pointer styles");
        auto *col = new QVBoxLayout(dialog);
        col->setContentsMargins(24, 20, 24, 20);
        col->setSpacing(10);
        auto *intro = new QLabel("Free pointer styles from their makers' GitHub pages. They go in "
                                 "~/.local/share/icons, so no password is needed.");
        intro->setObjectName("quiet");
        intro->setWordWrap(true);
        col->addWidget(intro);
        auto *listWidget = new QWidget;
        auto *list = new QVBoxLayout(listWidget);
        list->setContentsMargins(0, 0, 8, 0);
        list->setSpacing(10);
        auto *scroll = new QScrollArea;
        scroll->setWidget(listWidget);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        col->addWidget(scroll, 1);

        const QString iconsDir = QDir::homePath() + "/.local/share/icons";
        for (const CursorDownload &d : cursorDownloads()) {
            auto *row = new QHBoxLayout;
            row->addWidget(new QLabel(d.name), 1);
            auto *get = new QPushButton;
            get->setObjectName("chip");
            get->setCursor(Qt::PointingHandCursor);
            auto installed = [d] {
                for (const QString &id : d.ids)
                    if (findThemeDir(id).isEmpty())
                        return false;
                return true;
            };
            get->setText(installed() ? "Installed" : "Install");
            get->setEnabled(!installed());
            row->addWidget(get);
            list->addLayout(row);
            connect(get, &QPushButton::clicked, this, [this, get, d, iconsDir, installed] {
                get->setText("Downloading…");
                get->setEnabled(false);
                auto *p = new QProcess(this);
                connect(p, &QProcess::finished, this, [this, p, get, installed](int code) {
                    p->deleteLater();
                    const bool ok = code == 0 && installed();
                    get->setText(ok ? "Installed" : "Failed, try again");
                    get->setEnabled(!ok);
                    if (ok)
                        rescan();
                });
                p->start("sh", {"-c", kInstallCursorScript, "sh", d.url, iconsDir});
            });
        }
        list->addStretch();
        dialog->resize(520, 560);
        dialog->show();
    }

    // New styles arrived: rebuild the cards from what's installed now.
    void rescan()
    {
        for (ThemeCard *c : std::as_const(cards)) {
            cardGroup->removeButton(c);
            gridLayout->removeWidget(c);
            c->deleteLater();
        }
        cards.clear();
        columns = 0;
        buildCards();
        firstLoad = true;
        reloadFromFile();
    }

    void save()
    {
        ensureCursorRequired();
        writeCursorSetting(current);
        applyCursorLive(current);
    }

    QLabel *title, *hint, *fileLabel;
    QButtonGroup *sizeGroup, *cardGroup;
    QWidget *grid;
    QGridLayout *gridLayout;
    QScrollArea *scroll;
    QFileSystemWatcher *watcher;
    QList<ThemeCard *> cards;
    CursorSetting current;
    Palette pal;
    int columns = 0;
    bool firstLoad = true;
};

// ------------------------------------------------------------ slider

// Two things made the stock slider fiddly in a scrolling page: a click on the
// track only nudged it a step, and scrolling the page over one changed it.
// This one jumps to where you click and leaves the wheel to the page.
class Slider : public QSlider
{
public:
    Slider() : QSlider(Qt::Horizontal)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(28); // a bigger target than the 4px line
    }

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) {
            QStyleOptionSlider opt;
            initStyleOption(&opt);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
            if (!handle.contains(e->position().toPoint())) {
                const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
                const int span = groove.width() - handle.width();
                const int x = e->position().toPoint().x() - groove.x() - handle.width() / 2;
                setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), x, qMax(1, span)));
            }
        }
        // The handle is under the pointer now, so this starts a drag.
        QSlider::mousePressEvent(e);
    }

    void wheelEvent(QWheelEvent *e) override { e->ignore(); }
};

// ------------------------------------------------------------ lua settings

// The Hyprland pages edit Omarchy's own hand-written files (looknfeel.lua,
// input.lua), so instead of rewriting them whole they change one value in
// place and leave comments and everything else alone. This is just enough
// of a Lua reader to find `key = value` inside nested tables.

struct LuaSpan {
    int start = -1, end = -1;
};

struct LuaScan {
    QHash<QString, LuaSpan> values; // "general.gaps_in" -> the text after '='
    QHash<QString, int> tables;     // "general" -> just inside its '{'
};

static bool isIdentStart(QChar c) { return c.isLetter() || c == '_'; }
static bool isIdentChar(QChar c) { return c.isLetterOrNumber() || c == '_'; }

// Index just past a comment or string starting at i, or i if there's none.
static int skipLuaNoise(const QString &t, int i)
{
    const int n = t.size();
    if (t[i] == '-' && i + 1 < n && t[i + 1] == '-') {
        if (t.mid(i + 2, 2) == "[[") {
            const int close = t.indexOf("]]", i + 4);
            return close < 0 ? n : close + 2;
        }
        while (i < n && t[i] != '\n')
            ++i;
        return i;
    }
    if (t[i] == '"' || t[i] == '\'') {
        const QChar q = t[i++];
        while (i < n && t[i] != q && t[i] != '\n') {
            if (t[i] == '\\')
                ++i;
            ++i;
        }
        return qMin(n, i + 1);
    }
    return i;
}

static LuaScan scanLua(const QString &t)
{
    LuaScan out;
    struct Frame {
        QString name; // empty for a table with no key, like hl.config({
        int start;
    };
    QList<Frame> stack;
    auto pathWith = [&stack](const QString &key) {
        QStringList parts;
        for (const Frame &f : stack)
            if (!f.name.isEmpty())
                parts << f.name;
        if (!key.isEmpty())
            parts << key;
        return parts.join('.');
    };

    const int n = t.size();
    int i = 0;
    while (i < n) {
        const int skipped = skipLuaNoise(t, i);
        if (skipped != i) {
            i = skipped;
            continue;
        }
        const QChar c = t[i];
        if (isIdentStart(c) && (i == 0 || (!isIdentChar(t[i - 1]) && t[i - 1] != '.'))) {
            int j = i;
            while (j < n && isIdentChar(t[j]))
                ++j;
            int k = j;
            while (k < n && (t[k] == ' ' || t[k] == '\t'))
                ++k;
            if (k >= n || t[k] != '=' || (k + 1 < n && t[k + 1] == '=')) {
                i = j;
                continue;
            }
            const QString key = t.mid(i, j - i);
            int v = k + 1;
            while (v < n && (t[v] == ' ' || t[v] == '\t'))
                ++v;
            if (v < n && t[v] == '{') {
                stack.append({key, v});
                out.tables.insert(pathWith(QString()), v + 1);
                i = v + 1;
                continue;
            }
            // A plain value runs to the comma, line end or closing brace.
            int e = v, depth = 0;
            while (e < n) {
                if (t[e] == '-' && e + 1 < n && t[e + 1] == '-')
                    break;
                if (t[e] == '"' || t[e] == '\'') {
                    e = skipLuaNoise(t, e);
                    continue;
                }
                if (t[e] == '(' || t[e] == '{' || t[e] == '[')
                    ++depth;
                else if (t[e] == ')' || t[e] == '}' || t[e] == ']') {
                    if (depth == 0)
                        break;
                    --depth;
                } else if ((t[e] == ',' || t[e] == '\n') && depth == 0)
                    break;
                ++e;
            }
            int end = e;
            while (end > v && t[end - 1].isSpace())
                --end;
            out.values.insert(pathWith(key), {v, end});
            i = e;
            continue;
        }
        if (c == '{') {
            stack.append({QString(), -1});
        } else if (c == '}' && !stack.isEmpty()) {
            const Frame f = stack.takeLast();
            if (!f.name.isEmpty())
                out.values.insert(pathWith(f.name), {f.start, i + 1});
        }
        ++i;
    }
    return out;
}

static QString nestedLua(const QStringList &parts, const QString &value, const QString &indent,
                         const QString &unit)
{
    if (parts.size() == 1)
        return indent + parts[0] + " = " + value + ",\n";
    return indent + parts[0] + " = {\n" + nestedLua(parts.mid(1), value, indent + unit, unit)
           + indent + "},\n";
}

// Replaces the value at `path` if the file sets it; otherwise adds it to the
// deepest table that's already there, or a new hl.config block at the end.
static QString setLuaValue(QString t, const QString &path, const QString &value)
{
    const LuaScan scan = scanLua(t);
    if (scan.values.contains(path)) {
        const LuaSpan s = scan.values.value(path);
        return t.replace(s.start, s.end - s.start, value);
    }
    const QStringList parts = path.split('.');
    const QString unit = t.contains("\n\t") ? QStringLiteral("\t") : QStringLiteral("  ");
    for (int depth = parts.size() - 1; depth >= 1; --depth) {
        const QString table = parts.mid(0, depth).join('.');
        if (!scan.tables.contains(table))
            continue;
        const int at = scan.tables.value(table);
        const int lineStart = t.lastIndexOf('\n', at - 1) + 1;
        QString indent;
        for (int p = lineStart; p < at && (t[p] == ' ' || t[p] == '\t'); ++p)
            indent += t[p];
        return t.insert(at, "\n" + nestedLua(parts.mid(depth), value, indent + unit, unit).chopped(1));
    }
    if (!t.isEmpty() && !t.endsWith('\n'))
        t += '\n';
    return t + "\nhl.config({\n" + nestedLua(parts, value, unit, unit) + "})\n";
}

// What Hyprland is using right now, written the way the Lua file would
// write it, for settings the file leaves to Omarchy's defaults.
static QString hyprOption(const QString &path)
{
    QProcess p;
    p.start("hyprctl", {"getoption", QString(path).replace('.', ':')});
    if (!p.waitForFinished(1000))
        return QString();
    const QString first = QString::fromUtf8(p.readAllStandardOutput()).section('\n', 0, 0);
    const int colon = first.indexOf(':'); // "int: 40", "css gap data: 20 20 40 20"
    if (colon < 0)
        return QString();
    QString v = first.mid(colon + 1).trimmed();
    const QStringList sides = v.split(' ', Qt::SkipEmptyParts);
    if (first.startsWith("css gap") && sides.size() == 4)
        v = QString("{ top = %1, right = %2, bottom = %3, left = %4 }")
                .arg(sides[0], sides[1], sides[2], sides[3]);
    return v;
}

// A gap is one number for every side, or a table naming some of them.
static QList<int> parseGap(const QString &lua)
{
    bool ok = false;
    const int all = lua.trimmed().toInt(&ok);
    if (ok)
        return {all, all, all, all};
    QList<int> out{0, 0, 0, 0};
    const QStringList names{"top", "right", "bottom", "left"};
    static const QRegularExpression side("(top|right|bottom|left)\\s*=\\s*(\\d+)");
    auto it = side.globalMatch(lua);
    while (it.hasNext()) {
        const auto m = it.next();
        out[names.indexOf(m.captured(1))] = m.captured(2).toInt();
    }
    return out;
}

static QString gapLua(const QList<int> &g)
{
    if (g[0] == g[1] && g[1] == g[2] && g[2] == g[3])
        return QString::number(g[0]);
    return QString("{ top = %1, right = %2, bottom = %3, left = %4 }")
        .arg(g[0]).arg(g[1]).arg(g[2]).arg(g[3]);
}

// ------------------------------------------------------------ hyprland pages

struct HyprField {
    enum Kind { Int, Float, Bool, Gap, Choice };
    QString label;
    QString path; // in the Lua file; Hyprland's option name with ':' for '.'
    Kind kind;
    double min = 0, max = 1, step = 1;
    QString unit;
    QList<QPair<QString, QString>> choices = {}; // label, Lua value
};

static HyprField choiceField(const QString &label, const QString &path,
                             const QList<QPair<QString, QString>> &choices)
{
    HyprField f{label, path, HyprField::Choice, 0, 1, 1, {}};
    f.choices = choices;
    return f;
}

// A label for a settings row: wraps rather than pushing the controls off a
// narrow window.
static QLabel *rowLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    return l;
}

// A row of chips, one checked at a time, ids in order.
static QWidget *chipRow(const QStringList &labels, QButtonGroup *group)
{
    auto *w = new QWidget;
    auto *row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    for (int i = 0; i < labels.size(); ++i) {
        auto *b = new QPushButton(labels[i]);
        b->setCheckable(true);
        b->setObjectName("chip");
        b->setCursor(Qt::PointingHandCursor);
        group->addButton(b, i);
        row->addWidget(b);
    }
    row->addStretch();
    return w;
}

class HyprPage : public QWidget
{
public:
    HyprPage(const QString &titleText, const QString &hintText, const QString &fileName,
             const QList<HyprField> &list, QWidget *parent = nullptr)
        : QWidget(parent), file(QDir::homePath() + "/.config/hypr/" + fileName), fields(list)
    {
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(28, 24, 28, 20);
        outer->setSpacing(14);

        auto *title = new QLabel(titleText);
        title->setObjectName("title");
        title->setWordWrap(true);
        auto *hint = new QLabel(hintText);
        hint->setObjectName("quiet");
        hint->setWordWrap(true);
        outer->addWidget(title);
        outer->addWidget(hint);

        auto *body = new QWidget;
        grid = new QGridLayout(body);
        grid->setContentsMargins(0, 8, 12, 8);
        grid->setHorizontalSpacing(18);
        grid->setColumnStretch(0, 1);
        grid->setVerticalSpacing(6);
        grid->setAlignment(Qt::AlignTop);
        for (int f = 0; f < fields.size(); ++f) {
            const HyprField &field = fields[f];
            if (field.kind == HyprField::Bool) {
                Control c{f};
                c.toggle = new QButtonGroup(this);
                auto *chips = new QHBoxLayout;
                chips->setSpacing(6);
                for (const auto &[text, id] : {std::pair{"On", 1}, std::pair{"Off", 0}}) {
                    auto *b = new QPushButton(text);
                    b->setCheckable(true);
                    b->setObjectName("chip");
                    b->setCursor(Qt::PointingHandCursor);
                    c.toggle->addButton(b, id);
                    chips->addWidget(b);
                }
                grid->addWidget(rowLabel(field.label), row, 0);
                grid->addLayout(chips, row++, 1, Qt::AlignRight);
                grid->setRowMinimumHeight(row++, 10);
                connect(c.toggle, &QButtonGroup::idClicked, this, [this, f] { queue(f, 0); });
                controls.append(c);
                continue;
            }
            if (field.kind == HyprField::Choice) {
                Control c{f};
                c.toggle = new QButtonGroup(this);
                QStringList labels;
                for (const auto &choice : field.choices)
                    labels << choice.first;
                grid->addWidget(rowLabel(field.label), row++, 0, 1, 2);
                grid->addWidget(chipRow(labels, c.toggle), row++, 0, 1, 2);
                grid->setRowMinimumHeight(row++, 10);
                connect(c.toggle, &QButtonGroup::idClicked, this, [this, f] { queue(f, 0); });
                controls.append(c);
                continue;
            }
            const int sides = field.kind == HyprField::Gap ? 4 : 1;
            for (int s = 0; s < sides; ++s) {
                Control c{f, field.kind == HyprField::Gap ? s : -1};
                c.slider = new Slider;
                c.slider->setRange(0, qRound((field.max - field.min) / field.step));
                c.value = new QLabel;
                c.value->setObjectName("quiet");
                static const char *sideNames[] = {"top", "right", "bottom", "left"};
                const QString label = sides == 1 ? field.label
                                                 : QString("%1, %2").arg(field.label, sideNames[s]);
                // Name and number on one line, the slider full width below,
                // so it still fits when the window is tiled narrow.
                grid->addWidget(rowLabel(label), row, 0);
                grid->addWidget(c.value, row++, 1, Qt::AlignRight);
                grid->addWidget(c.slider, row++, 0, 1, 2);
                grid->setRowMinimumHeight(row++, 10);
                const int index = controls.size();
                connect(c.slider, &QSlider::valueChanged, this, [this, index] {
                    showValue(controls[index]);
                    queue(controls[index].field, controls[index].slider->isSliderDown() ? 400 : 150);
                });
                // Letting go saves at once rather than waiting out the timer.
                connect(c.slider, &QSlider::sliderReleased, this, [this] {
                    if (!pending.isEmpty())
                        saveTimer->start(0);
                });
                controls.append(c);
            }
        }

        auto *scroll = new QScrollArea;
        scroll->setWidget(body);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        outer->addWidget(scroll, 1);

        auto *footer = new QHBoxLayout;
        auto *notes = new QVBoxLayout;
        fileLabel = new QLabel(QString("Saved to %1").arg(QString(file).replace(QDir::homePath(), "~")));
        fileLabel->setObjectName("quiet");
        errorLabel = new QLabel;
        errorLabel->setObjectName("error");
        errorLabel->setWordWrap(true);
        errorLabel->hide();
        notes->addWidget(fileLabel);
        notes->addWidget(errorLabel);
        auto *openFile = new QPushButton("Open file");
        openFile->setObjectName("chip");
        openFile->setCursor(Qt::PointingHandCursor);
        footer->addLayout(notes, 1);
        footer->addWidget(openFile, 0, Qt::AlignTop);
        outer->addLayout(footer);
        connect(openFile, &QPushButton::clicked, this, [this] {
            if (!QProcess::startDetached("omarchy-launch-editor", {file}))
                QProcess::startDetached("xdg-open", {file});
        });

        // Dragging writes as it goes, a beat behind, so the desktop follows
        // the slider without a reload for every pixel.
        saveTimer = new QTimer(this);
        saveTimer->setSingleShot(true);
        connect(saveTimer, &QTimer::timeout, this, [this] { save(); });

        // Hand edits show up here. Saves replace the file, so the path is
        // re-added after each one; the timer folds a burst into one read.
        auto *loadTimer = new QTimer(this);
        loadTimer->setSingleShot(true);
        loadTimer->setInterval(200);
        connect(loadTimer, &QTimer::timeout, this, [this] { load(); });
        watcher = new QFileSystemWatcher(this);
        watcher->addPath(QFileInfo(file).absolutePath());
        if (QFileInfo::exists(file))
            watcher->addPath(file);
        connect(watcher, &QFileSystemWatcher::fileChanged, loadTimer, qOverload<>(&QTimer::start));
        connect(watcher, &QFileSystemWatcher::directoryChanged, this, [this, loadTimer] {
            if (!watcher->files().contains(file) && QFileInfo::exists(file)) {
                watcher->addPath(file);
                loadTimer->start();
            }
        });

        load();
    }

    // For a setting that isn't in the file, like the keyboard light.
    void addRow(const QString &label, QWidget *control, QWidget *value = nullptr)
    {
        grid->addWidget(rowLabel(label), row, 0);
        if (value)
            grid->addWidget(value, row, 1, Qt::AlignRight);
        grid->addWidget(control, ++row, 0, 1, 2);
        grid->setRowMinimumHeight(++row, 10);
        ++row;
    }

private:
    struct Control {
        int field;
        int side = -1; // which edge, for a gap
        QSlider *slider = nullptr;
        QLabel *value = nullptr;
        QButtonGroup *toggle = nullptr;
    };

    double sliderValue(const Control &c) const
    {
        const HyprField &f = fields[c.field];
        return f.min + c.slider->value() * f.step;
    }

    static QString number(double v, int decimals)
    {
        QString s = QString::number(v, 'f', decimals);
        if (s.contains('.')) {
            while (s.endsWith('0'))
                s.chop(1);
            if (s.endsWith('.'))
                s.chop(1);
        }
        return s;
    }

    int decimals(const HyprField &f) const
    {
        return f.kind == HyprField::Float ? qMax(0, qCeil(-std::log10(f.step) - 1e-9)) : 0;
    }

    void showValue(const Control &c)
    {
        const HyprField &f = fields[c.field];
        c.value->setText(number(sliderValue(c), decimals(f)) + (f.unit.isEmpty() ? "" : " " + f.unit));
    }

    QString luaValue(int field) const
    {
        const HyprField &f = fields[field];
        if (f.kind == HyprField::Gap) {
            QList<int> g{0, 0, 0, 0};
            for (const Control &c : controls)
                if (c.field == field)
                    g[c.side] = qRound(sliderValue(c));
            return gapLua(g);
        }
        for (const Control &c : controls) {
            if (c.field != field)
                continue;
            if (f.kind == HyprField::Bool)
                return c.toggle->checkedId() == 1 ? "true" : "false";
            if (f.kind == HyprField::Choice)
                return f.choices.value(qMax(0, c.toggle->checkedId())).second;
            return number(sliderValue(c), decimals(f));
        }
        return QString();
    }

    void queue(int field, int delay)
    {
        pending.insert(field);
        saveTimer->start(delay);
    }

    void save()
    {
        QFile in(file);
        QString text;
        if (in.open(QIODevice::ReadOnly | QIODevice::Text))
            text = QString::fromUtf8(in.readAll());
        in.close();
        const LuaScan scan = scanLua(text);
        QStringList missing;
        for (int f : std::as_const(pending)) {
            // A bare name is a `local` the file has to define already, like
            // monitors.lua's omarchy_monitor_scale; adding one wouldn't work.
            if (!fields[f].path.contains('.') && !scan.values.contains(fields[f].path)) {
                missing << fields[f].path;
                continue;
            }
            text = setLuaValue(text, fields[f].path, luaValue(f));
        }
        pending.clear();
        if (!missing.isEmpty()) {
            errorLabel->setText(QString("%1 has no %2 line to change.")
                                    .arg(QFileInfo(file).fileName(), missing.join(", ")));
            errorLabel->show();
        }
        QSaveFile out(file);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
            return;
        out.write(text.toUtf8());
        if (!out.commit())
            return;

        // Reload so it applies now, then say so if Hyprland didn't like it.
        auto *reload = new QProcess(this);
        connect(reload, &QProcess::finished, this, [this, reload] {
            reload->deleteLater();
            auto *check = new QProcess(this);
            connect(check, &QProcess::finished, this, [this, check] {
                check->deleteLater();
                showErrors(QString::fromUtf8(check->readAllStandardOutput()).trimmed());
            });
            check->start("hyprctl", {"configerrors"});
        });
        reload->start("hyprctl", {"reload"});
    }

    void showErrors(const QString &errors)
    {
        const QString name = QFileInfo(file).fileName();
        QStringList mine;
        for (const QString &line : errors.split('\n'))
            if (line.contains(name))
                mine << line.section(QDir::homePath() + "/.config/hypr/", -1);
        errorLabel->setText("Hyprland didn't accept this: " + mine.join("; "));
        errorLabel->setVisible(!mine.isEmpty());
    }

    // Each setting comes from the file if it's there, otherwise from what
    // Hyprland is using, which is Omarchy's default.
    void load()
    {
        QFile in(file);
        QString text;
        if (in.open(QIODevice::ReadOnly | QIODevice::Text))
            text = QString::fromUtf8(in.readAll());
        const LuaScan scan = scanLua(text);
        QHash<int, QString> lua;
        for (int f = 0; f < fields.size(); ++f) {
            const QString &path = fields[f].path;
            if (scan.values.contains(path)) {
                const LuaSpan s = scan.values.value(path);
                lua.insert(f, text.mid(s.start, s.end - s.start));
            } else {
                lua.insert(f, hyprOption(path));
            }
        }
        for (Control &c : controls) {
            const HyprField &f = fields[c.field];
            const QString v = lua.value(c.field).trimmed();
            if (f.kind == HyprField::Choice) {
                // Matched ignoring spaces, so "{4,3}" is the "{ 4, 3 }" chip.
                QString bare = v;
                bare.remove(' ');
                QSignalBlocker block(c.toggle);
                c.toggle->setExclusive(false);
                for (int i = 0; i < f.choices.size(); ++i) {
                    QString want = f.choices[i].second;
                    c.toggle->button(i)->setChecked(want.remove(' ') == bare
                                                    || (want.toDouble() != 0 && want.toDouble() == bare.toDouble()));
                }
                c.toggle->setExclusive(true);
                continue;
            }
            if (c.toggle) {
                const bool on = v == "true" || v == "1";
                QSignalBlocker block(c.toggle);
                c.toggle->button(on ? 1 : 0)->setChecked(true);
                continue;
            }
            // Don't yank the handle out from under a drag in progress.
            if (c.slider->isSliderDown() || pending.contains(c.field))
                continue;
            double n;
            if (f.kind == HyprField::Gap)
                n = parseGap(v)[c.side];
            else if (f.kind == HyprField::Int && v.startsWith('{'))
                n = parseGap(v)[0]; // gaps_in written as a table
            else
                n = v.toDouble();
            QSignalBlocker block(c.slider);
            c.slider->setValue(qRound((qBound(f.min, n, f.max) - f.min) / f.step));
            showValue(c);
        }
    }

    const QString file;
    const QList<HyprField> fields;
    QList<Control> controls;
    QGridLayout *grid;
    int row = 0;
    QSet<int> pending;
    QTimer *saveTimer;
    QLabel *fileLabel, *errorLabel;
    QFileSystemWatcher *watcher;
};

// Lights aren't Hyprland settings: brightnessctl changes them directly, the
// same as the kbhigh/kblow aliases and the brightness keys.
static QString lightDevice(const QString &deviceClass, const QString &nameHas)
{
    QProcess p;
    p.start("brightnessctl", {"--list", "--machine-readable"});
    if (!p.waitForFinished(1000))
        return QString();
    for (const QString &line : QString::fromUtf8(p.readAllStandardOutput()).split('\n')) {
        const QStringList f = line.split(','); // name,class,current,percent,max
        if (f.size() >= 5 && f[1] == deviceClass && f[0].contains(nameHas))
            return f[0];
    }
    return QString();
}

static int lightPercent(const QString &device)
{
    QProcess p;
    p.start("brightnessctl", {"--device", device, "--machine-readable"});
    if (!p.waitForFinished(1000))
        return -1;
    const QStringList f = QString::fromUtf8(p.readAllStandardOutput()).trimmed().split(',');
    return f.size() >= 5 ? QString(f[3]).remove('%').toInt() : -1;
}

// A slider for a value that lives outside any file. `read` is asked again
// every couple of seconds, since keys and other apps change these too.
static void addLiveSlider(HyprPage *page, const QString &label, int min, int max, int step,
                          const QString &unit, std::function<int()> read,
                          std::function<void(int)> write)
{
    auto *slider = new Slider;
    slider->setRange(min / step, max / step);
    auto *value = new QLabel;
    value->setObjectName("quiet");
    page->addRow(label, slider, value);
    auto show = [value, step, unit](int pos) { value->setText(QString("%1 %2").arg(pos * step).arg(unit)); };

    auto *apply = new QTimer(page);
    apply->setSingleShot(true);
    apply->setInterval(80);
    // When we last wrote; reading back too soon can catch the old value
    // and snap the handle back.
    auto *wrote = new QElapsedTimer;
    QObject::connect(slider, &QObject::destroyed, [wrote] { delete wrote; });
    QObject::connect(apply, &QTimer::timeout, page, [slider, step, write, wrote] {
        write(slider->value() * step);
        wrote->start();
    });
    QObject::connect(slider, &QSlider::valueChanged, page, [show, apply](int pos) {
        show(pos);
        apply->start();
    });

    auto refresh = [slider, step, read, show, apply, wrote] {
        if (slider->isSliderDown() || apply->isActive()
            || (wrote->isValid() && wrote->elapsed() < 2500))
            return;
        const int now = read();
        if (now < 0 || qRound(qreal(now) / step) == slider->value())
            return;
        QSignalBlocker block(slider);
        slider->setValue(qRound(qreal(now) / step));
        show(slider->value());
    };
    auto *poll = new QTimer(page);
    poll->setInterval(2000);
    QObject::connect(poll, &QTimer::timeout, page, [slider, refresh] {
        if (slider->isVisible())
            refresh();
    });
    poll->start();
    show(slider->value());
    refresh();
}

static void addLight(HyprPage *page, const QString &label, const QString &deviceClass,
                     const QString &nameHas, int min)
{
    const QString device = lightDevice(deviceClass, nameHas);
    if (device.isEmpty())
        return;
    addLiveSlider(page, label, min, 100, 5, "%", [device] { return lightPercent(device); },
                  [device](int v) {
                      QProcess::startDetached("brightnessctl", {"--quiet", "--device", device, "set",
                                                                QString::number(v) + "%"});
                  });
}

// Omarchy's Super+Ctrl+Backspace switch: a state file that makes a lone
// window keep a shape instead of filling a wide screen. Off is no file; the
// shortcut and this row flip the same file.
static QString aspectTogglePath()
{
    return QDir::homePath() + "/.local/state/omarchy/toggles/hypr/single-window-aspect-ratio.lua";
}

static void addAspectRow(HyprPage *page)
{
    static const QList<QPair<QString, QString>> shapes = {
        {"Fill", ""}, {"Square", "1, 1"}, {"4:3", "4, 3"}, {"3:2", "3, 2"}, {"16:9", "16, 9"}};
    auto *group = new QButtonGroup(page);
    QStringList labels;
    for (const auto &shape : shapes)
        labels << shape.first;
    page->addRow("A lone window's shape", chipRow(labels, group));

    auto load = [group] {
        QFile f(aspectTogglePath());
        int pick = 0;
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            static const QRegularExpression ratio("single_window_aspect_ratio\\s*=\\s*\\{\\s*(\\d+)\\s*,\\s*(\\d+)");
            const auto m = ratio.match(QString::fromUtf8(f.readAll()));
            for (int i = 1; i < shapes.size() && m.hasMatch(); ++i)
                if (shapes[i].second == m.captured(1) + ", " + m.captured(2))
                    pick = i;
        }
        QSignalBlocker block(group);
        group->button(pick)->setChecked(true);
    };
    QObject::connect(group, &QButtonGroup::idClicked, page, [](int i) {
        if (i == 0) {
            QFile::remove(aspectTogglePath());
        } else {
            QDir().mkpath(QFileInfo(aspectTogglePath()).absolutePath());
            QSaveFile f(aspectTogglePath());
            if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
                return;
            f.write(QString("-- Avoid overly wide single-window layouts on wide screens.\n"
                            "hl.config({\n  layout = {\n    single_window_aspect_ratio = { %1 },\n  },\n})\n")
                        .arg(shapes[i].second)
                        .toUtf8());
            f.commit();
        }
        QProcess::startDetached("hyprctl", {"reload"});
    });
    // The shortcut can flip it behind our back.
    auto *poll = new QTimer(page);
    poll->setInterval(2000);
    QObject::connect(poll, &QTimer::timeout, page, load);
    poll->start();
    load();
}

static HyprPage *makeWindowsPage()
{
    auto *page = new HyprPage(
        "Windows",
        "Spacing, borders and corners for tiled windows. Changes apply as you drag.",
        "looknfeel.lua",
        {
            {"Gap between windows", "general.gaps_in", HyprField::Int, 0, 40, 1, "px"},
            {"Screen edge", "general.gaps_out", HyprField::Gap, 0, 80, 1, "px"},
            {"Border", "general.border_size", HyprField::Int, 0, 10, 1, "px"},
            {"Corner rounding", "decoration.rounding", HyprField::Int, 0, 30, 1, "px"},
            {"Dim unfocused windows", "decoration.dim_inactive", HyprField::Bool, 0, 1, 1, {}},
            {"Dim amount", "decoration.dim_strength", HyprField::Float, 0, 1, 0.05, {}},
            {"Animations", "animations.enabled", HyprField::Bool, 0, 1, 1, {}},
        });
    addAspectRow(page);
    return page;
}

static double gtkTextScale()
{
    QProcess p;
    p.start("gsettings", {"get", "org.gnome.desktop.interface", "text-scaling-factor"});
    if (!p.waitForFinished(1000))
        return -1;
    bool ok = false;
    const double v = QString::fromUtf8(p.readAllStandardOutput()).trimmed().toDouble(&ok);
    return ok ? v : -1;
}

// `omarchy font set` changes the terminals, the bar and fontconfig's
// monospace together, so the picker just runs it.
static void addFontRow(HyprPage *page)
{
    QProcess list, current;
    list.start("omarchy", {"font", "list"});
    current.start("omarchy", {"font", "current"});
    if (!list.waitForFinished(3000) || !current.waitForFinished(3000))
        return;
    const QStringList fonts =
        QString::fromUtf8(list.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    if (fonts.isEmpty())
        return;
    auto *combo = new QComboBox;
    combo->addItems(fonts);
    combo->setMaxVisibleItems(16);
    combo->setCursor(Qt::PointingHandCursor);
    combo->setCurrentText(QString::fromUtf8(current.readAllStandardOutput()).trimmed());
    page->addRow("System font", combo);
    QObject::connect(combo, &QComboBox::textActivated, page, [](const QString &font) {
        QProcess::startDetached("omarchy", {"font", "set", font});
    });
}

static HyprPage *makeDisplayPage()
{
    auto *page = new HyprPage(
        "Display",
        "Brightness, how big everything is drawn, and the font.",
        "monitors.lua",
        {
            choiceField("Screen scale (makes everything bigger)", "omarchy_monitor_scale",
                        {{"Auto", "\"auto\""}, {"100%", "1"}, {"125%", "1.25"}, {"150%", "1.5"}, {"200%", "2"}}),
        });
    addLight(page, "Screen brightness", "backlight", "", 5);
    addLiveSlider(
        page, "Text size (GTK apps)", 80, 160, 5, "%",
        [] {
            const double v = gtkTextScale();
            return v < 0 ? -1 : qRound(v * 100);
        },
        [](int v) {
            QProcess::startDetached("gsettings", {"set", "org.gnome.desktop.interface",
                                                  "text-scaling-factor", QString::number(v / 100.0)});
        });
    addFontRow(page);
    return page;
}

static HyprPage *makeInputPage()
{
    auto *page = new HyprPage(
        "Keyboard & touchpad",
        "How keys repeat and light up, how fast the pointer moves, and how the touchpad scrolls.",
        "input.lua",
        {
            {"Key repeat speed", "input.repeat_rate", HyprField::Int, 10, 80, 1, "/ sec"},
            {"Delay before repeat", "input.repeat_delay", HyprField::Int, 150, 800, 10, "ms"},
            {"Pointer speed", "input.sensitivity", HyprField::Float, -1, 1, 0.05, {}},
            {"Natural scrolling", "input.touchpad.natural_scroll", HyprField::Bool, 0, 1, 1, {}},
            {"Touchpad scroll speed", "input.touchpad.scroll_factor", HyprField::Float, 0.05, 2, 0.05, {}},
            {"Ignore touchpad while typing", "input.touchpad.disable_while_typing", HyprField::Bool, 0, 1, 1, {}},
            {"Two-finger click is right-click", "input.touchpad.clickfinger_behavior", HyprField::Bool, 0, 1, 1, {}},
        });
    addLight(page, "Keyboard light", "leds", "kbd_backlight", 0);
    return page;
}

// ------------------------------------------------------------ window

// A stack only as wide as the page showing, not its widest page, so a
// narrow tiled window doesn't clip the slim ones.
class PageStack : public QStackedWidget
{
public:
    QSize minimumSizeHint() const override
    {
        return currentWidget() ? currentWidget()->minimumSizeHint() : QSize();
    }
    QSize sizeHint() const override
    {
        return currentWidget() ? currentWidget()->sizeHint() : QSize();
    }
};

class Omasettings : public QWidget
{
public:
    Omasettings()
    {
        setObjectName("root");
        setWindowTitle("Omasettings");
        resize(1040, 680);

        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);

        sidebar = new QListWidget;
        sidebar->setObjectName("sidebar");
        sidebar->setFixedWidth(230);
        sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sidebar->setTextElideMode(Qt::ElideRight);
        sidebar->setFocusPolicy(Qt::NoFocus);
        stack = new PageStack;
        row->addWidget(sidebar);
        row->addWidget(stack, 1);

        pointer = new PointerPage;
        addPage("Mouse pointer", pointer);
        addPage("Windows", makeWindowsPage());
        addPage("Keyboard & touchpad", makeInputPage());
        addPage("Display", makeDisplayPage());

        connect(sidebar, &QListWidget::currentRowChanged, this, [this](int i) {
            stack->setCurrentIndex(i);
            stack->updateGeometry();
        });
        sidebar->setCurrentRow(0);

        applyPalette();
        // Re-tint live when the Omarchy theme changes.
        themeWatcher = new QFileSystemWatcher(this);
        themeWatcher->addPath(omarchyColorsPath());
        connect(themeWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &f) {
            applyPalette();
            if (!themeWatcher->files().contains(f))
                themeWatcher->addPath(f);
        });
    }

    void showPage(const QString &word)
    {
        for (int i = 0; i < sidebar->count(); ++i)
            if (sidebar->item(i)->text().contains(word, Qt::CaseInsensitive)) {
                sidebar->setCurrentRow(i);
                return;
            }
    }

protected:
    // Tiled narrow, the pages need the room more than the sidebar does.
    void resizeEvent(QResizeEvent *e) override
    {
        sidebar->setFixedWidth(qBound(130, int(width() * 0.3), 230));
        QWidget::resizeEvent(e);
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape || e->matches(QKeySequence::Quit))
            close();
        else
            QWidget::keyPressEvent(e);
    }

    // A plain QWidget subclass ignores a stylesheet background unless it
    // draws PE_Widget itself.
    void paintEvent(QPaintEvent *) override
    {
        QStyleOption opt;
        opt.initFrom(this);
        QPainter p(this);
        style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
    }

private:
    void addPage(const QString &name, QWidget *page)
    {
        sidebar->addItem(name);
        stack->addWidget(page);
    }

    void applyPalette()
    {
        const Palette p = loadOmarchyPalette();
        pointer->setColors(p);
        const QColor quiet = mix(p.page, p.ink, 0.55);
        const QColor raised = mix(p.page, p.ink, 0.05);
        const QColor line = mix(p.page, p.ink, 0.15);
        setFont(QFont(uiFontFamily(), 11));
        setStyleSheet(
            // The font goes in the sheet too: setFont alone missed rows added
            // after a page was built.
            QString("QWidget { font-family: \"%7\"; font-size: 11pt; }"
                    "#root, QStackedWidget, QScrollArea, QScrollArea > QWidget > QWidget"
                    "  { background: %1; color: %2; }"
                    "QLabel { background: transparent; color: %2; }"
                    "QLabel#title { font-size: 20pt; font-weight: bold; }"
                    "QLabel#quiet { color: %4; }"
                    "#sidebar { font-family: \"%7\"; font-size: 11pt; background: %5; color: %2; border: none;"
                    "  border-right: 1px solid %6; padding: 16px 8px; outline: none; }"
                    "#sidebar::item { padding: 10px 12px; border-radius: 6px; }"
                    "#sidebar::item:selected { background: %6; color: %3; }"
                    "QPushButton#chip { background: %5; color: %2; border: 1px solid %6;"
                    "  border-radius: 6px; padding: 6px 14px; }"
                    "QPushButton#chip:hover { border-color: %4; }"
                    "QPushButton#chip:checked { background: %3; color: %1; border-color: %3; }"
                    "QScrollBar:vertical { background: transparent; width: 10px; }"
                    "QScrollBar::handle:vertical { background: %6; border-radius: 5px;"
                    "  min-height: 32px; }"
                    "QScrollBar::add-line, QScrollBar::sub-line { height: 0; }"
                    "QLabel#error { color: #e06c75; }"
                    "QSlider::groove:horizontal { height: 4px; background: %6; border-radius: 2px; }"
                    "QSlider::sub-page:horizontal { background: %3; border-radius: 2px; }"
                    "QSlider::handle:horizontal { background: %2; width: 16px; height: 16px;"
                    "  margin: -6px 0; border-radius: 8px; }"
                    "QComboBox { background: %5; color: %2; border: 1px solid %6;"
                    "  border-radius: 6px; padding: 6px 10px; }"
                    "QComboBox:hover { border-color: %4; }"
                    "QComboBox QAbstractItemView { background: %5; color: %2;"
                    "  selection-background-color: %3; selection-color: %1; border: 1px solid %6; }"
                    "QToolTip { background: %5; color: %2; border: 1px solid %6; }")
                .arg(p.page.name(), p.ink.name(), p.accent.name(), quiet.name(),
                     raised.name(), line.name(), uiFontFamily()));
    }

    QListWidget *sidebar;
    QStackedWidget *stack;
    PointerPage *pointer;
    QFileSystemWatcher *themeWatcher;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("omarchy");
    QCoreApplication::setApplicationName("omasettings");
    QGuiApplication::setDesktopFileName("omasettings");

    // `omasettings pointer <theme> [size]` sets it without opening a window,
    // through the same file and live switch the page uses.
    const QStringList args = QCoreApplication::arguments();
    if (args.size() >= 2 && args[1] == "pointer") {
        CursorSetting s = readCursorSetting();
        if (args.size() < 3) {
            QTextStream(stdout) << s.theme << " " << s.size << "\n";
            return 0;
        }
        QSet<QString> known;
        for (const CursorTheme &t : findCursorThemes())
            known.insert(t.id);
        if (!known.contains(args[2])) {
            QTextStream(stderr) << "No cursor theme named " << args[2] << "\n";
            return 1;
        }
        s.theme = args[2];
        if (args.size() >= 4 && args[3].toInt() > 0)
            s.size = args[3].toInt();
        ensureCursorRequired();
        if (!writeCursorSetting(s))
            return 1;
        applyCursorLive(s);
        return 0;
    }

    Omasettings w;
    // `--page windows` opens on that page; any word of its name will do.
    const int page = args.indexOf("--page");
    if (page > 0 && page + 1 < args.size())
        w.showPage(args[page + 1]);
    w.show();
    return app.exec();
}
