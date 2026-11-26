// LibraryView.swift: the Media panel.
//   Header: "MEDIA" (or, inside a folder, back and the folder's name), the import spinner, the Text and Solid
//   buttons (click to add at the playhead, or drag to the timeline), and Import.
//   A search field: plain words search names; words in "quotes" search what's said.
//   Inside a linked folder that's gone missing: where to find it.
//   Folders and items, as Thumbnails or a List (Settings > General, or the panel's right-click menu). Moving the
//   mouse across a video's picture scrubs through it, in the viewer too. A click picks (Shift for a range, Ctrl to
//   add or take away); a double-click adds an item at the playhead or opens a folder; drag items to the
//   timeline (several picked go end to end); right-click for more. Files and folders dropped on the panel come in.
#pragma once
#include "Controller.h"
#include <QLineEdit>
#include <QWidget>

class QScrollArea;
class QLabel;

namespace er {

/// How the Media panel shows its items (MediaLayout, kept under "mediaLayout").
enum class MediaLayout { thumbnails, list };
MediaLayout mediaLayout();
void setMediaLayout(MediaLayout l);

/// The items: folders and media, drawn and handled as one widget.
class MediaGrid : public QWidget {
    Q_OBJECT
public:
    MediaGrid(Controller& c, QWidget* parent = nullptr);
    void setSearch(const QString& s);
    /// Lays the items out again for the panel's width; returns the height they take.
    void relayout();
    struct Item {
        bool folder = false;
        QString folderPath;
        MediaAsset asset;
        QRectF rect, picture;
        std::optional<QString> place;
        MediaPick pick() const { return folder ? MediaPick::ofFolder(folderPath) : MediaPick::ofAsset(asset.id); }
    };
    const std::vector<Item>& items() const { return items_; }
    bool empty() const { return items_.empty(); }
signals:
    void contentChanged();
    void folderOpened();
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    std::vector<MediaAsset> shown(const std::vector<MediaAsset>& assets) const;
    std::optional<size_t> itemAt(QPointF p) const;
    void drawPicture(QPainter& p, const Item& it, bool compact);
    void openFolder(const QString& folder);
    void assetMenu(const Item& it, QPoint at);
    void folderMenu(const Item& it, QPoint at);
    double thumbWidth() const;
    Controller& c_;
    QString search_;
    std::vector<Item> items_;
    std::optional<size_t> hover_;
    /// Hover-scrubbing a video's picture: where across it, 0…1.
    std::optional<double> scrub_;
    QPointF press_;
    std::optional<size_t> pressed_;
    bool layingOut_ = false;
};

class LibraryPanel : public QWidget {
    Q_OBJECT
public:
    explicit LibraryPanel(Controller& c, QWidget* parent = nullptr);
    /// The panel's own menu: add a text or solid at the playhead, import, and List or Thumbnails.
    static void panelMenu(Controller& c, QWidget* parent, QPoint at);
protected:
    void paintEvent(QPaintEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
private:
    void refresh();
    void reveal(const MediaPick& target);
    void showDropped();
    Controller& c_;
    QWidget* header_;
    QWidget* dropOverlay_;
    QWidget* body_;
    QLineEdit* search_;
    QWidget* missing_;
    QLabel* empty_;
    QLabel* noMatch_;
    MediaGrid* grid_;
    QScrollArea* scroll_;
    bool dropTargeted_ = false;
};

} // namespace er
