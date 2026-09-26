// omasettings - click-through settings for an Omarchy desktop.
// Every page writes a plain config file under ~/.config, so anything set here
// can also be edited by hand, and hand edits show up here.
//
// Pages so far: Mouse pointer.
//
//   omasettings                        open the window
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

    void save()
    {
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

// ------------------------------------------------------------ window

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
        sidebar->setFixedWidth(200);
        sidebar->setFocusPolicy(Qt::NoFocus);
        stack = new QStackedWidget;
        row->addWidget(sidebar);
        row->addWidget(stack, 1);

        pointer = new PointerPage;
        addPage("Mouse pointer", pointer);

        connect(sidebar, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);
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

protected:
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
            QString("#root, QStackedWidget, QScrollArea, QScrollArea > QWidget > QWidget"
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
        if (!writeCursorSetting(s))
            return 1;
        applyCursorLive(s);
        return 0;
    }

    Omasettings w;
    w.show();
    return app.exec();
}
