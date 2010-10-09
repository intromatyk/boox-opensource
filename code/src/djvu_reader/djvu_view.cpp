#include "djvu_view.h"
#include "djvu_model.h"
#ifdef BUILD_FOR_ARM
#include <QtGui/qwsdisplay_qws.h>
#include <QtGui/qscreen_qws.h>
#endif

namespace djvu_reader
{

static const int OVERLAP_DISTANCE = 80;
static const int SLIDE_TIME_INTERVAL = 5000;

static RotateDegree getSystemRotateDegree()
{
    int degree = 0;
#ifdef BUILD_FOR_ARM
    degree = QScreen::instance()->transformOrientation();
#endif
    return static_cast<RotateDegree>(degree);
}

DjvuView::DjvuView(QWidget *parent)
    : BaseView(parent, Qt::FramelessWindowHint)
    , model_(0)
    , restore_count_(0)
    , bookmark_image_(0)
    , current_waveform_(onyx::screen::instance().defaultWaveform())
{
    connect(&slide_timer_, SIGNAL(timeout()), this, SLOT(slideShowNextPage()));

    update_bookmark_timer_.setSingleShot(true);
    update_bookmark_timer_.setInterval(0);
    connect(&update_bookmark_timer_, SIGNAL(timeout()), this, SLOT(onUpdateBookmark()));

    connect(&status_mgr_, SIGNAL(stylusChanged(const int)), this, SLOT(onStylusChanges(const int)));
    connect(&sketch_proxy_, SIGNAL(requestUpdateScreen()), this, SLOT(onRequestUpdateScreen()));

    connect(&render_proxy_, SIGNAL(pageRenderReady(DjVuPagePtr)), this, SLOT(onPageRenderReady(DjVuPagePtr)));
    connect(&render_proxy_, SIGNAL(contentAreaReady(const int, const QRect &)),
            this, SLOT(onContentAreaReady(const int, const QRect &)));

    // set drawing area to sketch agent
    sketch_proxy_.setDrawingArea(this);
    sketch_proxy_.setWidgetOrient(getSystemRotateDegree());
}

DjvuView::~DjvuView(void)
{
}

void DjvuView::attachModel(BaseModel *model)
{
    if (model_ == model)
    {
        return;
    }

    // Record the model.
    model_ = static_cast<DjvuModel*>(model);

    // connect the signals
    connect(model_, SIGNAL(docReady()), this, SLOT(onDocReady()));
    connect(model_, SIGNAL(docError(QString, QString, int)), this, SLOT(onDocError(QString, QString, int)));
    connect(model_, SIGNAL(docInfo(QString)), this, SLOT(onDocInfo(QString)));
    connect(model_, SIGNAL(docPageReady()), this, SLOT(onDocPageReady()));
    connect(model_, SIGNAL(docThumbnailReady(int)), this, SLOT(onDocThumbnailReady(int)));
    connect(model_, SIGNAL(docIdle()), this, SLOT(onDocIdle()));
    connect(model_, SIGNAL(requestSaveAllOptions()), this, SLOT(onSaveAllOptions()));
}

void DjvuView::deattachModel()
{
    disconnect(model_, SIGNAL(docReady()), this, SLOT(onDocReady()));
    disconnect(model_, SIGNAL(docError(QString, QString, int)), this, SLOT(onDocError(QString, QString, int)));
    disconnect(model_, SIGNAL(docInfo(QString)), this, SLOT(onDocInfo(QString)));
    disconnect(model_, SIGNAL(docPageReady()), this, SLOT(onDocPageReady()));
    disconnect(model_, SIGNAL(docThumbnailReady(int)), this, SLOT(onDocThumbnailReady(int)));
    disconnect(model_, SIGNAL(docIdle()), this, SLOT(onDocIdle()));
    model_ = 0;
}

void DjvuView::onSaveAllOptions()
{
    // save all of the configurations
    saveConfiguration(model_->getConf());

    // save & close the sketch document
    sketch_proxy_.save();
}

/// Save the configuration
bool DjvuView::saveConfiguration(Configuration & conf)
{
    // save the reading progress
    QString progress("%1 / %2");
    conf.info.mutable_progress() = progress.arg(cur_page_ + 1).arg(model_->getPagesTotalNumber());

    conf.options[CONFIG_PAGE_LAYOUT] = read_mode_;
    return layout_->saveConfiguration(conf);
}

void DjvuView::initLayout()
{
    if (read_mode_ == CONTINUOUS_LAYOUT)
    {
        layout_.reset(new ContinuousPageLayout(view_setting_.rotate_orient,
                                               view_setting_.zoom_setting));
    }
    else
    {
        layout_.reset(new SinglePageLayout(view_setting_.rotate_orient,
                                           view_setting_.zoom_setting));
    }

    connect(layout_.get(), SIGNAL(layoutDoneSignal()), this, SLOT(onLayoutDone()));
    connect(layout_.get(), SIGNAL(needPageSignal(const int)), this, SLOT(onNeedPage(const int)));
    connect(layout_.get(), SIGNAL(needContentAreaSignal(const int)), this, SLOT(onNeedContentArea(const int)));

    layout_->setMargins(cur_margin_);
    layout_->setWidgetArea(QRect(0, 0, size().width(), size().height()));
}

void DjvuView::onContentAreaReady(const int page_number, const QRect & content_area)
{
    layout_->setContentArea(page_number, content_area);
}

bool DjvuView::generateRenderSetting(vbf::PagePtr page, RenderSettingPtr setting)
{
    if (layout_->zoomSetting() == ZOOM_HIDE_MARGIN)
    {
        // always set clipping to be true
        setting->setClipImage(true);

        // get the displaying area of content and out-bounding rectangle
        QRect content_area;
        QRect clip_area;
        if (!vbf::getDisplayContentAreas(page->contentArea(),
                                         page->actualArea(),
                                         page->zoomValue(),
                                         layout_->rotateDegree(),
                                         clip_area,
                                         content_area))
        {
            // render task will calculate the displaying areas later
            setting->setContentArea(page->actualArea());
            setting->setClipArea(clip_area);
        }
        else
        {
            setting->setContentArea(content_area);
            setting->setClipArea(clip_area);
        }
    }
    else
    {
        setting->setContentArea(page->displayArea());
        setting->setClipImage(false);
    }
    return true;
}

void DjvuView::onLayoutDone()
{
    // clear the previous visible pages
    clearVisiblePages();
    layout_->getVisiblePages(layout_pages_);
    if (layout_pages_.empty())
    {
        return;
    }

    if (status_mgr_.isErasing() || status_mgr_.isSketching())
    {
        updateSketchProxy();
    }

    // send the render request for first page
    vbf::PagePtr page = layout_pages_.front();
    int previous_page = cur_page_;
    cur_page_ = page->key();
    if (cur_page_ != previous_page)
    {
        sketch_proxy_.save();
    }

    // send the render requests
    PageRenderSettings render_settings;
    VisiblePages::iterator idx = layout_pages_.begin();
    for (; idx != layout_pages_.end(); idx++)
    {
        PagePtr visible_page = *idx;
        RenderSettingPtr render_setting(new RenderSetting());
        generateRenderSetting(visible_page, render_setting);
        render_settings[visible_page->key()] = render_setting;

        // load sketch page
        sketch::PageKey page_key;
        page_key.setNum(visible_page->key());
        sketch_proxy_.loadPage(model_->path(), page_key, QString());
    }
    render_proxy_.render(render_settings, model_->document());
}

void DjvuView::onNeedPage(const int page_number)
{
    shared_ptr<ddjvu_pageinfo_t> page_info = model_->getPageInfo(page_number);
    if (page_info != 0)
    {
        QRect rect;
        rect.setWidth(page_info->width);
        rect.setHeight(page_info->height);
        layout_->setPage(page_number, rect);
    }
}

void DjvuView::onNeedContentArea(const int page_number)
{
    render_proxy_.requirePageContentArea(page_number, model_->document());
}

void DjvuView::resetLayout()
{
    // NOTE: The document should be ready when calling this function
    layout_->clearPages();
    layout_->setFirstPageNumber(model_->firstPageNumber());
    layout_->setLastPageNumber(model_->getPagesTotalNumber() - 1);
    layout_->update();
}

/// Load the configurations and update the view
bool DjvuView::loadConfiguration(Configuration & conf)
{
    if (conf.options.empty())
    {
        return false;
    }

    bool ok = false;
    read_mode_ = static_cast<PageLayoutType>(conf.options[CONFIG_PAGE_LAYOUT].toInt(&ok));
    if (!ok)
    {
        return false;
    }
    return true;
}

void DjvuView::onDocReady()
{
    // load the configuration from model
    if (!loadConfiguration(model_->getConf()))
    {
        read_mode_ = PAGE_LAYOUT;
    }

    // initialize the pages layout by configurations.
    // If the configurations are invalid, the layout is initialized by default.
    initLayout();
    layout_->loadConfiguration(model_->getConf());
    resetLayout();

    //QWidget* thumbnail_view = down_cast<MainWindow*>(parentWidget())->getView(THUMBNAIL_VIEW);
    //if (thumbnail_view != 0)
    //{
    //    // attach the model with thumbnail view
    //    down_cast<ThumbnailView*>(thumbnail_view)->setTotalNumber(model_->getPagesTotalNumber());
    //}
}

void DjvuView::onDocError(QString msg, QString file_name, int line_no)
{
}

void DjvuView::onDocInfo(QString msg)
{
}

void DjvuView::onDocPageReady()
{
}

void DjvuView::onDocThumbnailReady(int page_num)
{
}

void DjvuView::onDocIdle()
{
}

void DjvuView::attachMainWindow(MainWindow *main_window)
{
    connect(this, SIGNAL(currentPageChanged(const int, const int)),
            main_window, SLOT(handlePositionChanged(const int, const int)));
    connect(this, SIGNAL(fullScreen(bool)),
            main_window, SLOT(handleFullScreen(bool)));
    connect(this, SIGNAL(itemStatusChanged(const StatusBarItemType, const int)),
            main_window, SLOT(handleItemStatusChanged(const StatusBarItemType, const int)));
    connect(this, SIGNAL(requestUpdateParent(bool)),
            main_window, SLOT(handleRequestUpdate(bool)));
    connect(this, SIGNAL(popupJumpPageDialog()),
            main_window, SLOT(handlePopupJumpPageDialog()));

    connect(main_window, SIGNAL(pagebarClicked(const int, const int)),
            this, SLOT(onPagebarClicked(const int, const int)));
    connect(main_window, SIGNAL(popupContextMenu()),
            this, SLOT(onPopupMenu()));

    status_mgr_.setStatus(ID_PAN, FUNC_SELECTED);
}

void DjvuView::deattachMainWindow(MainWindow *main_window)
{
    disconnect(this, SIGNAL(currentPageChanged(const int, const int)),
               main_window, SLOT(handlePositionChanged(const int, const int)));
    disconnect(this, SIGNAL(fullScreen(bool)),
               main_window, SLOT(handleFullScreen(bool)));
    disconnect(this, SIGNAL(itemStatusChanged(const StatusBarItemType, const int)),
               main_window, SLOT(handleItemStatusChanged(const StatusBarItemType, const int)));
    disconnect(this, SIGNAL(requestUpdateParent(bool)),
               main_window, SLOT(handleRequestUpdate(bool)));
    disconnect(this, SIGNAL(popupJumpPageDialog()),
               main_window, SLOT(handlePopupJumpPageDialog()));

    disconnect(main_window, SIGNAL(pagebarClicked(const int, const int)),
               this, SLOT(onPagebarClicked(const int, const int)));
    disconnect(main_window, SIGNAL(popupContextMenu()),
               this, SLOT(onPopupMenu()));
}

void DjvuView::attachTreeView(TreeViewDialog *tree_view)
{
}

void DjvuView::deattachTreeView(TreeViewDialog *tree_view)
{
}

void DjvuView::onStylusChanges(const int type)
{
    switch (type)
    {
    case ID_SKETCHING:
    case ID_ERASING:
        attachSketchProxy();
        break;
    default:
        deattachSketchProxy();
        break;
    }
    emit itemStatusChanged(STYLUS, type);
}

void DjvuView::onRequestUpdateScreen()
{
    onyx::screen::instance().enableUpdate(false);
    repaint();
    onyx::screen::instance().enableUpdate(true);
    onyx::screen::instance().updateWidget( this, onyx::screen::ScreenProxy::GU );
}

void DjvuView::returnToLibrary()
{
    qApp->exit();
}

bool DjvuView::flip(int direction)
{
    // TODO. Implement Me
    return false;
}

void DjvuView::onPageRenderReady(DjVuPagePtr page)
{
    if (restore_count_ > 1)
    {
        qDebug("Restore Left:%d", restore_count_);
        restore_count_--;
        return;
    }

    if (sys::SysStatus::instance().isSystemBusy())
    {
        // if it is the first time rendering, set busy to be false
        sys::SysStatus::instance().setSystemBusy( false );
    }

    // remove the mapping page in layout pages
    bool found = false;
    VisiblePagesIter begin = layout_pages_.begin();
    VisiblePagesIter end = layout_pages_.end();
    VisiblePagesIter idx = begin;
    for (; idx != end; ++idx)
    {
        if (page->pageNum() == (*idx)->key())
        {
            layout_pages_.erase(idx);
            found = true;
            break;
        }
    }

    if (!found)
    {
        qDebug("Page %d is out of date", page->pageNum());
        return;
    }

    // set the waveform by current paging mode
    if (display_pages_.size() > 0)
    {
        onyx::screen::instance().setDefaultWaveform(onyx::screen::ScreenProxy::GU);
    }
    else
    {
        onyx::screen::instance().setDefaultWaveform(current_waveform_);
    }

    display_pages_.push_back(page);

    // retrieve the next one and send render request
    if (layout_pages_.empty())
    {
        // set current page in page bar
        updateCurrentPage(layout_->getCurrentPage());
        if (restore_count_ <= 0)
        {
            // save the reading history besides the restored one
            saveReadingContext();
        }
        else // restore_count_ == 1
        {
            restore_count_ = 0;
        }
    }

    // redraw the Qt image buffer and make sure mandatory update the view
    update();

    // rollback to current default mode after update
    if (layout_pages_.isEmpty())
    {
        onyx::screen::instance().flush(0, onyx::screen::ScreenProxy::INVALID);
        onyx::screen::instance().setDefaultWaveform(current_waveform_);
    }
}

void DjvuView::gotoPage(const int page_number)
{
    layout_->jump(page_number);
}

void DjvuView::onPagebarClicked(const int percent, const int value)
{
    gotoPage(value);
}

void DjvuView::onPopupMenu()
{
    if ( onyx::screen::instance().defaultWaveform() == onyx::screen::ScreenProxy::DW )
    {
        // stop fastest update mode to get better image quality.
        if ( current_waveform_ == onyx::screen::ScreenProxy::DW )
        {
            current_waveform_ = onyx::screen::ScreenProxy::GC;
        }
        onyx::screen::instance().setDefaultWaveform(current_waveform_);
    }

    ui::PopupMenu menu(this);
    updateActions();
    if ( !status_mgr_.isSlideShow() )
    {
        menu.addGroup(&zoom_setting_actions_);
        if (SysStatus::instance().hasTouchScreen())
        {
            menu.addGroup(&sketch_actions_);
        }
        menu.addGroup(&view_actions_);
    }
    menu.addGroup(&reading_tools_actions_);
    menu.setSystemAction(&system_actions_);

    if (menu.popup() != QDialog::Accepted)
    {
        QApplication::processEvents();
        return;
    }

    // To solve update issue. At first, we disabled the screen update
    // the Qt frame buffer is synchronised by using processEvents.
    // Finally, the screen update is enabled.
    onyx::screen::instance().enableUpdate(false);
    QApplication::processEvents();
    onyx::screen::instance().enableUpdate(true);

    QAction * group = menu.selectedCategory();
    bool disable_update = true;
    if (group == zoom_setting_actions_.category())
    {
        disable_update = !zooming(zoom_setting_actions_.getSelectedZoomValue());
    }
    else if (group == view_actions_.category())
    {
        ViewActionsType type = INVALID_VIEW_TYPE;
        int value = -1;

        type = view_actions_.getSelectedValue(value);
        switch (type)
        {
        case VIEW_ROTATION:
            rotate();
            break;
        case VIEW_PAGE_LAYOUT:
            switchLayout(static_cast<PageLayoutType>(value));
            disable_update = false;
            break;
        default:
            break;
        }
    }
    else if (group == reading_tools_actions_.category())
    {
        int tool = reading_tools_actions_.selectedTool();
        switch (tool)
        {
        case SLIDE_SHOW:
            {
                if (status_mgr_.isSlideShow())
                {
                    stopSlideShow();
                }
                else
                {
                    startSlideShow();
                }
            }
            break;
        case TOC_VIEW_TOOL:
            {
                displayOutlines(true);
            }
            break;
        case SCROLL_PAGE:
            {
                status_mgr_.setStatus( ID_PAN, FUNC_SELECTED );
                disable_update = false;
            }
            break;
        case GOTO_PAGE:
            {
                emit popupJumpPageDialog();
            }
            break;
        case ADD_BOOKMARK:
            {
                disable_update = addBookmark();
            }
            break;
        case DELETE_BOOKMARK:
            {
                disable_update = deleteBookmark();
            }
            break;
        case SHOW_ALL_BOOKMARKS:
            {
                displayBookmarks();
            }
            break;
        case PREVIOUS_VIEW:
            {
                back();
            }
            break;
        case NEXT_VIEW:
            {
                forward();
            }
            break;
        default:
            break;
        }
    }
    else if (group == sketch_actions_.category())
    {
        int value = -1;
        bool checked = false;
        SketchActionsType type = sketch_actions_.getSelectedValue(value, checked);
        switch (type)
        {
        case SKETCH_MODE:
            setSketchMode(static_cast<SketchMode>(value), checked);
            break;
        case SKETCH_COLOR:
            setSketchColor(static_cast<SketchColor>(value));
            break;
        case SKETCH_SHAPE:
            setSketchShape(static_cast<SketchShape>(value));
            break;
        default:
            break;
        }
        disable_update = false;
    }
    else if (group == system_actions_.category())
    {
        SystemAction system_action = system_actions_.selected();
        switch (system_action)
        {
        case RETURN_TO_LIBRARY:
            returnToLibrary();
            break;
        case SCREEN_UPDATE_TYPE:
            onyx::screen::instance().updateWidget(this, onyx::screen::ScreenProxy::GU, true);
            onyx::screen::instance().toggleWaveform();
            current_waveform_ = onyx::screen::instance().defaultWaveform();
            disable_update = false;
            break;
        case MUSIC:
            openMusicPlayer();
            break;
        case ROTATE_SCREEN:
            rotate();
            break;
        default:
            break;
        }
    }

    if (!disable_update)
    {
        emit requestUpdateParent(true);
    }
}

void DjvuView::slideShowNextPage()
{
    int current = cur_page_;
    int total   = model_->getPagesTotalNumber();
    if ((++current) >= total)
    {
        current = 0;
    }
    gotoPage(current);
}

void DjvuView::switchLayout(PageLayoutType mode)
{
    if (read_mode_ == mode)
    {
        return;
    }

    read_mode_ = mode;
    initLayout();
    gotoPage(cur_page_);
    resetLayout();
}


void DjvuView::mousePressEvent(QMouseEvent *me)
{
    switch (me->button())
    {
    case Qt::LeftButton:
        if (status_mgr_.isZoomIn())
        {
            zoomInPress(me);
        }
        else if(status_mgr_.isPan())
        {
            panPress(me);
        }
        else if (status_mgr_.isSketching())
        {
            // the mouse events has been eaten by sketch proxy
        }
        else if (status_mgr_.isErasing())
        {
            // the mouse events has been eaten by sketch proxy
        }
        break;
    case Qt::RightButton:
        {
            onPopupMenu();
        }
        break;
    default:
        break;
    }
    me->accept();

}

void DjvuView::mouseReleaseEvent(QMouseEvent *me)
{
    static const int MOVE_ERROR = 5;
    switch (me->button())
    {
    case Qt::LeftButton:
        if (status_mgr_.isZoomIn())
        {
            zoomInRelease(me);
        }
        else if (status_mgr_.isPan())
        {
            panRelease(me);
        }
        else if (status_mgr_.isSketching())
        {
            // the mouse events has been eaten by sketch proxy
        }
        else if (status_mgr_.isErasing())
        {
            // the mouse events has been eaten by sketch proxy
        }
        else if (status_mgr_.isSlideShow())
        {
            stopSlideShow();
        }
        break;
    case Qt::RightButton:
        {
        }
        break;
    default:
        break;
    }
    me->accept();
}

void DjvuView::mouseMoveEvent(QMouseEvent *me)
{
    if (status_mgr_.isZoomIn())
    {
        zoomInMove(me);
    }
    else if (status_mgr_.isSketching())
    {
        // the mouse events has been eaten by sketch proxy
    }
    else if (status_mgr_.isErasing())
    {
        // the mouse events has been eaten by sketch proxy
    }
    me->accept();
}

bool DjvuView::hitTestBookmark(const QPoint &point)
{
    if (layout_ == 0 || bookmark_image_ == 0)
    {
        return false;
    }

    QPoint pt(rect().width()- bookmark_image_->width(), 0);
    QPoint bookmark_size(bookmark_image_->width(), bookmark_image_->height());
    QRect bookmark_rect(pt, QPoint(pt + bookmark_size));
    if (bookmark_rect.contains(point))
    {
        VisiblePages visible_pages;
        layout_->getVisiblePages(visible_pages);
        if (!visible_pages.isEmpty())
        {
            int start = visible_pages.front()->key();
            int end   = visible_pages.back()->key();
            if (model_->hasBookmark(start, end))
            {
                update_bookmark_timer_.start();
                return true;
            }
        }
    }
    return false;
}

bool DjvuView::hitTest(const QPoint &point)
{
    return hitTestBookmark(point);
}

void DjvuView::onUpdateBookmark()
{
    sys::SysStatus::instance().setSystemBusy(false);
    if (layout_ == 0)
    {
        return;
    }

    int start = -1;
    int end   = -1;
    QString previous_title;
    VisiblePages visible_pages;
    layout_->getVisiblePages(visible_pages);
    if (!visible_pages.isEmpty())
    {
        start = visible_pages.front()->key();
        end   = visible_pages.back()->key();
        previous_title = model_->getFirstBookmarkTitle(start, end);
    }
    else
    {
        return;
    }

    if ( notes_dialog_ == 0 )
    {
        notes_dialog_.reset( new NotesDialog( QString(), this ) );
        notes_dialog_->updateTitle(tr("Name Bookmark"));
    }

    int ret = notes_dialog_->popup(previous_title);
    if (ret == QDialog::Accepted)
    {
        QString content = notes_dialog_->inputText();
        model_->updateBookmark(start, end, content);
    }
}

void DjvuView::scroll(int offset_x, int offset_y)
{
    if ( status_mgr_.isSlideShow() )
    {
        return;
    }

    int x = offset_x;
    int y = offset_y;
    if (isLandscape())
    {
        x = offset_y;
        y = offset_x;
    }
    layout_->scroll(x, y);
}

void DjvuView::keyReleaseEvent(QKeyEvent *ke)
{
    int offset = 0;
    switch(ke->key())
    {
    case Qt::Key_PageDown:
    case Qt::Key_Down:
        {
            offset = (height() - OVERLAP_DISTANCE);
            if (isLandscape())
            {
                offset = (width() - OVERLAP_DISTANCE);
            }
            scroll(0, offset);
        }
        break;
    case Qt::Key_Right:
        {
            offset = (width() - OVERLAP_DISTANCE);
            if (isLandscape())
            {
                offset = (height() - OVERLAP_DISTANCE);
            }
            scroll(offset, 0);
        }
        break;
    case Qt::Key_PageUp:
    case Qt::Key_Up:
        {
            offset = -(height() - OVERLAP_DISTANCE);
            if (isLandscape())
            {
                offset = - (width() - OVERLAP_DISTANCE);
            }
            scroll(0, offset);
        }
        break;
    case Qt::Key_Left:
        {
            offset = - (width() - OVERLAP_DISTANCE);
            if (isLandscape())
            {
                offset = - (height() - OVERLAP_DISTANCE);
            }
            scroll(offset, 0);
        }
        break;
    case Qt::Key_Z:
        {
            selectionZoom();
        }
        break;
    case Qt::Key_B:
        {
            zooming(ZOOM_TO_PAGE);
        }
        break;
    case Qt::Key_P:
        {
            enableScrolling();
        }
        break;
    case Qt::Key_W:
        {
            zooming(ZOOM_TO_WIDTH);
        }
        break;
    case Qt::Key_H:
        {
            zooming(ZOOM_TO_HEIGHT);
        }
        break;
    case Qt::Key_S:
        {
            if (!slide_timer_.isActive())
            {
                startSlideShow();
            }
            else
            {
                stopSlideShow();
            }
        }
        break;
    case Qt::Key_F3:
        {
        }
        break;
    case Qt::Key_F4:
        {
        }
        break;
    case Qt::Key_Escape:
        {
            if ( status_mgr_.isSlideShow() )
            {
                stopSlideShow();
            }
            else
            {
                returnToLibrary();
            }
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        {
            emit popupJumpPageDialog();
        }
        break;
    case ui::Device_Menu_Key:
        {
            onPopupMenu();
        }
        break;
    default:
        break;
    }
    ke->accept();
}

void DjvuView::selectionZoom()
{
    status_mgr_.setStatus(ID_ZOOM_IN, FUNC_SELECTED);
}

void DjvuView::enableScrolling()
{
    status_mgr_.setStatus( ID_PAN, FUNC_SELECTED );
}

void DjvuView::paintEvent(QPaintEvent *pe)
{
    int count = display_pages_.size();
    QPainter painter(this);
    for (int i = 0; i < count; ++i)
    {
        DjVuPagePtr page = display_pages_.get_page(i);
        paintPage(painter, page);
    }
    paintBookmark(painter);
}

/// update the current page
void DjvuView::updateCurrentPage(const int page_number)
{
    cur_page_ = page_number;
    emit currentPageChanged(cur_page_, model_->getPagesTotalNumber());
}

void DjvuView::resizeEvent(QResizeEvent *re)
{
    if (layout_ != 0 &&
        layout_->setWidgetArea(QRect(0,
                                     0,
                                     re->size().width(),
                                     re->size().height())))
    {
        layout_->update();
    }
}

void DjvuView::attachSketchProxy()
{
    if (status_mgr_.isErasing())
    {
        sketch_proxy_.setMode(MODE_ERASING);
    }
    else if (status_mgr_.isSketching())
    {
        sketch_proxy_.setMode(MODE_SKETCHING);
    }
    sketch_proxy_.attachWidget(this);
    updateSketchProxy();
}

void DjvuView::deattachSketchProxy()
{
    sketch_proxy_.deattachWidget(this);
}

void DjvuView::updateSketchProxy()
{
    // deactivate all pages
    sketch_proxy_.deactivateAll();

    // activate visible pages
    vbf::VisiblePages visible_pages;
    layout_->getVisiblePages(visible_pages);
    VisiblePagesIter idx = visible_pages.begin();
    while (idx != visible_pages.end())
    {
        vbf::PagePtr page_layout = *idx;
        int page_number = page_layout->key();
        QPoint page_pos;
        if (layout_->getContentPos(page_number, page_pos))
        {
            QRect page_area;
            page_area.setTopLeft(page_pos);
            page_area.setSize(page_layout->displayArea().size());
            if (layout_->zoomSetting() == ZOOM_HIDE_MARGIN)
            {
                QRect content_area;
                if (vbf::getDisplayContentAreas(page_layout->contentArea(),
                                                page_layout->actualArea(),
                                                page_layout->zoomValue(),
                                                layout_->rotateDegree(),
                                                content_area,
                                                page_area))
                {
                    QPoint content_pos;
                    if (vbf::getDisplayContentPosition(page_layout->contentArea(),
                                                       page_layout->actualArea(),
                                                       page_layout->zoomValue(),
                                                       layout_->rotateDegree(),
                                                       content_pos))
                    {
                        page_area.moveTo(page_area.topLeft() - content_pos);
                    }
                }
            }

            // update zoom factor
            sketch_proxy_.setZoom(page_layout->zoomValue() * ZOOM_ACTUAL);
            sketch_proxy_.setContentOrient(layout_->rotateDegree());
            sketch_proxy_.setWidgetOrient(getSystemRotateDegree());

            sketch::PageKey page_key;

            // the page number of any image is 0
            page_key.setNum(page_number);
            sketch_proxy_.activatePage(model_->path(), page_key);
            sketch_proxy_.updatePageDisplayRegion(model_->path(), page_key, page_area);
        }
        idx++;
    }
}

bool DjvuView::updateActions()
{
    // Reading Tools
    std::vector<ReadingToolsType> reading_tools;
    if ( !status_mgr_.isSlideShow() )
    {
        reading_tools.push_back(SCROLL_PAGE);
        if (model_->hasOutlines())
        {
            reading_tools.push_back(TOC_VIEW_TOOL);
        }
        reading_tools.push_back(GOTO_PAGE);
    }
    reading_tools.push_back(SLIDE_SHOW);
    reading_tools_actions_.generateActions(reading_tools);
    reading_tools_actions_.setActionStatus(SLIDE_SHOW, status_mgr_.isSlideShow());
    reading_tools_actions_.setActionStatus(SCROLL_PAGE, status_mgr_.isPan());

    if ( !status_mgr_.isSlideShow() )
    {
        reading_tools.clear();
        reading_tools.push_back(ADD_BOOKMARK);
        reading_tools.push_back(DELETE_BOOKMARK);
        reading_tools.push_back(SHOW_ALL_BOOKMARKS);
        reading_tools_actions_.generateActions(reading_tools, true);

        reading_tools.clear();
        reading_tools.push_back(PREVIOUS_VIEW);
        reading_tools.push_back(NEXT_VIEW);
        reading_tools_actions_.generateActions(reading_tools, true);

        // Zoom Settings
        std::vector<ZoomFactor> zoom_settings;
        zoom_settings.push_back(ZOOM_HIDE_MARGIN);
        zoom_settings.push_back(ZOOM_TO_PAGE);
        zoom_settings.push_back(ZOOM_TO_WIDTH);
        zoom_settings.push_back(ZOOM_TO_HEIGHT);
        if (SysStatus::instance().hasTouchScreen())
        {
            zoom_settings.push_back(ZOOM_SELECTION);
        }
        zoom_settings.push_back(75.0f);
        zoom_settings.push_back(100.0f);
        zoom_settings.push_back(125.0f);
        zoom_settings.push_back(150.0f);
        zoom_settings.push_back(175.0f);
        zoom_settings.push_back(200.0f);
        zoom_settings.push_back(300.0f);
        zoom_settings.push_back(400.0f);
        zoom_setting_actions_.generateActions(zoom_settings);
        zoom_setting_actions_.setCurrentZoomValue(view_setting_.zoom_setting);

        // View Settings
        PageLayouts page_layouts;
        page_layouts.push_back(PAGE_LAYOUT);
        page_layouts.push_back(CONTINUOUS_LAYOUT);
        view_actions_.generatePageLayoutActions(page_layouts, read_mode_);

        // set sketch mode
        sketch_actions_.clear();
        SketchModes     sketch_modes;
        SketchColors    sketch_colors;
        SketchShapes    sketch_shapes;

        sketch_modes.push_back(MODE_SKETCHING);
        sketch_modes.push_back(MODE_ERASING);

        sketch_colors.push_back(SKETCH_COLOR_WHITE);
        //sketch_colors.push_back(SKETCH_COLOR_LIGHT_GRAY);
        //sketch_colors.push_back(SKETCH_COLOR_DARK_GRAY);
        sketch_colors.push_back(SKETCH_COLOR_BLACK);

        sketch_shapes.push_back(SKETCH_SHAPE_0);
        sketch_shapes.push_back(SKETCH_SHAPE_1);
        sketch_shapes.push_back(SKETCH_SHAPE_2);
        sketch_shapes.push_back(SKETCH_SHAPE_3);
        sketch_shapes.push_back(SKETCH_SHAPE_4);

        sketch_actions_.generateSketchMode(sketch_modes);
        if (status_mgr_.isSketching())
        {
            sketch_actions_.setSketchMode(MODE_SKETCHING, true);
        }
        else if (status_mgr_.isErasing())
        {
            sketch_actions_.setSketchMode(MODE_ERASING, true);
        }

        if (!sketch_colors.empty())
        {
            sketch_actions_.generateSketchColors(sketch_colors, sketch_proxy_.getColor());
        }
        if (!sketch_shapes.empty())
        {
            sketch_actions_.generateSketchShapes(sketch_shapes, sketch_proxy_.getShape());
        }
        if (!status_mgr_.isSketching())
        {
            sketch_actions_.setSketchColor( INVALID_SKETCH_COLOR );
            sketch_actions_.setSketchShape( INVALID_SKETCH_SHAPE );
        }
    }

    system_actions_.generateActions();
    return true;
}

void DjvuView::generateZoomSettings( std::vector<ZoomFactor> & zoom_settings )
{
}

void DjvuView::displayOutlines( bool )
{
#ifdef MAIN_WINDOW_TOC_ON
    QWidget* tree_view = dynamic_cast<MainWindow*>(parentWidget())->getView(TOC_VIEW);
    if (tree_view == 0)
    {
        return;
    }

    down_cast<MainWindow*>(parentWidget())->activateView(TOC_VIEW);
    down_cast<TreeViewDialog*>(tree_view)->setModel( model_->getOutlineModel() );
    down_cast<TreeViewDialog*>(tree_view)->initialize( tr("Table of Contents") );
#else
    QStandardItemModel * outline_model = model_->getOutlineModel();
    if (outline_model == 0)
    {
        qDebug("No Outlines");
        return;
    }

    TreeViewDialog outline_view(this);
    outline_view.setModel(outline_model);
    outline_view.tree().showHeader(true);

    QVector<int> percentages;
    percentages.push_back(80);
    percentages.push_back(20);
    outline_view.tree().setColumnWidth(percentages);
    int ret = outline_view.popup( tr("Table of Contents") );

    // Returned from the TOC view
    onyx::screen::instance().enableUpdate( false );
    QApplication::processEvents();
    onyx::screen::instance().enableUpdate( true );

    if (ret != QDialog::Accepted)
    {
        return;
    }

    QModelIndex index = outline_view.selectedItem();
    if ( !index.isValid() )
    {
        return;
    }

    QString dest = model_->getDestByTOCIndex(index);
    if (!dest.isEmpty())
    {
        sys::SysStatus::instance().setSystemBusy(true, false);
        int dest_page = dest.toInt();
        gotoPage(dest_page - 1);
    }
#endif
}

bool DjvuView::zooming( double zoom_setting )
{
    view_setting_.zoom_setting = zoom_setting;
    if (zoom_setting == ZOOM_TO_PAGE)
    {
        layout_->zoomToBestFit();
    }
    else if (zoom_setting == ZOOM_TO_WIDTH)
    {
        layout_->zoomToWidth();
    }
    else if (zoom_setting == ZOOM_TO_HEIGHT)
    {
        layout_->zoomToHeight();
    }
    else if (zoom_setting == ZOOM_SELECTION)
    {
        selectionZoom();
        return false;
    }
    else if (zoom_setting == ZOOM_HIDE_MARGIN)
    {
        layout_->zoomToVisible();
    }
    else
    {
        layout_->setZoom(zoom_setting);
    }
    return true;
}

void DjvuView::zoomInPress( QMouseEvent *me )
{
    current_waveform_ = onyx::screen::instance().defaultWaveform();
    onyx::screen::instance().setDefaultWaveform(onyx::screen::ScreenProxy::DW);
    stroke_area_.initArea(me->pos());
    if (rubber_band_ == 0)
    {
        rubber_band_.reset(new QRubberBand(QRubberBand::Rectangle, this));
    }
    rubber_band_->setGeometry(QRect(stroke_area_.getOriginPosition(),
                                    QSize()));
    rubber_band_->show();
}

void DjvuView::zoomInMove( QMouseEvent *me )
{
    stroke_area_.expandArea(me->pos());
    rubber_band_->setGeometry(QRect(stroke_area_.getOriginPosition(),
                                    me->pos()).normalized());
}

void DjvuView::zoomIn(const QRect &zoom_rect)
{
    layout_->zoomIn(zoom_rect);
    view_setting_.zoom_setting = layout_->zoomSetting();
}

void DjvuView::zoomInRelease( QMouseEvent *me )
{
    stroke_area_.expandArea(me->pos());
    rubber_band_->hide();

    // clear the background
    onyx::screen::instance().flush(0, onyx::screen::ScreenProxy::GU);

    // return to previous waveform
    onyx::screen::instance().setDefaultWaveform(current_waveform_);

    sys::SysStatus::instance().setSystemBusy(true);
    zoomIn(stroke_area_.getRect());
    status_mgr_.setStatus(ID_ZOOM_IN, FUNC_NORMAL);
}

void DjvuView::panPress( QMouseEvent *me )
{
    pan_area_.setStartPoint(me->pos());
}

void DjvuView::panMove( QMouseEvent *me )
{
}

void DjvuView::panRelease( QMouseEvent *me )
{
    pan_area_.setEndPoint(me->pos());
    int sys_offset = sys::SystemConfig::direction( pan_area_.getStart(), pan_area_.getEnd() );
    int offset_x = 0, offset_y = 0;
    pan_area_.getOffset(offset_x, offset_y);
    if ( sys_offset == 0 )
    {
        hitTest(me->pos());
    }
    else
    {
        scroll(offset_x, offset_y);
    }
}

void DjvuView::setSketchMode( const SketchMode mode, bool selected )
{
    FunctionStatus s = selected ? FUNC_SELECTED : FUNC_NORMAL;
    FunctionID id = mode == MODE_SKETCHING ? ID_SKETCHING : ID_ERASING;
    status_mgr_.setStatus(id, s);
    sketch_proxy_.setMode(mode);
}

void DjvuView::setSketchColor( const SketchColor color )
{
    sketch_proxy_.setColor(color);
    status_mgr_.setStatus(ID_SKETCHING, FUNC_SELECTED);
}

void DjvuView::setSketchShape( const SketchShape shape )
{
    sketch_proxy_.setShape(shape);
    status_mgr_.setStatus(ID_SKETCHING, FUNC_SELECTED);
}

void DjvuView::paintPage(QPainter & painter, DjVuPagePtr page)
{
    if (page->image() == 0 || page->renderNeeded() || layout_ == 0)
    {
        return;
    }

    vbf::PagePtr page_layout = layout_->getPage(page->pageNum());
    if (page_layout == 0)
    {
        qDebug("The layout is not ready!");
        return;
    }

    QPoint cur_pos;
    if (layout_->getContentPos(page->pageNum(), cur_pos))
    {
        // draw content of page
        if (layout_->zoomSetting() != ZOOM_HIDE_MARGIN)
        {
            painter.drawImage(cur_pos, *(page->image()));
        }
        else
        {
            RenderSetting render_setting;
            if (render_proxy_.getPageRenderSetting(page->pageNum(), render_setting))
            {
                painter.drawImage(cur_pos, *(page->image()), render_setting.clipArea());
            }
        }
    }
    paintSketches(painter, page->pageNum());
}

void DjvuView::paintSketches( QPainter & painter, int page_no )
{
    QPoint page_pos;
    if (!layout_->getContentPos(page_no, page_pos))
    {
        return;
    }

    // update zoom factor
    vbf::PagePtr page_layout = layout_->getPage(page_no);
    QRect page_area(page_pos, page_layout->displayArea().size());
    if (layout_->zoomSetting() == ZOOM_HIDE_MARGIN)
    {
        QRect content_area;
        if (vbf::getDisplayContentAreas(page_layout->contentArea(),
                                        page_layout->actualArea(),
                                        page_layout->zoomValue(),
                                        layout_->rotateDegree(),
                                        content_area,
                                        page_area))
        {
            QPoint content_pos;
            if (vbf::getDisplayContentPosition(page_layout->contentArea(),
                                               page_layout->actualArea(),
                                               page_layout->zoomValue(),
                                               layout_->rotateDegree(),
                                               content_pos))
            {
                page_area.moveTo(page_area.topLeft() - content_pos);
            }
        }
    }

    sketch_proxy_.setZoom(page_layout->zoomValue() * ZOOM_ACTUAL);
    sketch_proxy_.setContentOrient(layout_->rotateDegree());
    sketch_proxy_.setWidgetOrient(getSystemRotateDegree());

    // draw sketches in this page
    // the page number of any image is 0
    sketch::PageKey page_key;
    page_key.setNum(page_no);
    sketch_proxy_.updatePageDisplayRegion(model_->path(), page_key, page_area);
    sketch_proxy_.paintPage(model_->path(), page_key, painter);
}

void DjvuView::paintBookmark( QPainter & painter )
{
    if (layout_ == 0)
    {
        return;
    }

    VisiblePages visible_pages;
    layout_->getVisiblePages(visible_pages);

    if (!visible_pages.isEmpty())
    {
        int start = visible_pages.front()->key();
        int end   = visible_pages.back()->key();
        if (model_->hasBookmark(start, end))
        {
            if (bookmark_image_ == 0)
            {
                bookmark_image_.reset(new QImage(":/images/bookmark_flag.png"));
            }
            QPoint pt(rect().width()- bookmark_image_->width(), 0);
            painter.drawImage(pt, *bookmark_image_);
        }
    }
}

void DjvuView::displayBookmarks()
{
    QStandardItemModel bookmarks_model;
    model_->getBookmarksModel( bookmarks_model );

    TreeViewDialog bookmarks_view( this );
    bookmarks_view.setModel( &bookmarks_model );
    bookmarks_view.tree().showHeader(true);

    QVector<int> percentages;
    percentages.push_back(80);
    percentages.push_back(20);
    bookmarks_view.tree().setColumnWidth(percentages);
    int ret = bookmarks_view.popup( tr("Bookmarks") );

    // Returned from the TOC view
    onyx::screen::instance().enableUpdate( false );
    QApplication::processEvents();
    onyx::screen::instance().enableUpdate( true );

    if (ret != QDialog::Accepted)
    {
        return;
    }

    QModelIndex index = bookmarks_view.selectedItem();
    if ( !index.isValid() )
    {
        return;
    }

    sys::SysStatus::instance().setSystemBusy(true, false);
    QStandardItem *item = bookmarks_model.itemFromIndex( index );
    gotoPage(item->data().toInt());
}

bool DjvuView::addBookmark()
{
    // get the beginning of current screen
    VisiblePages visible_pages;
    layout_->getVisiblePages(visible_pages);
    if (!visible_pages.isEmpty())
    {
        int start = visible_pages.front()->key();
        int end = visible_pages.back()->key();
        if (model_->addBookmark(start, end))
        {
            update();
            update_bookmark_timer_.start();
            return true;
        }
    }
    return false;
}

bool DjvuView::deleteBookmark()
{
    // get the beginning of current screen
    VisiblePages visible_pages;
    layout_->getVisiblePages(visible_pages);
    if (!visible_pages.isEmpty())
    {
        int start = visible_pages.front()->key();
        int end = visible_pages.back()->key();
        if (model_->deleteBookmark(start, end))
        {
            update();
            return true;
        }
    }
    return false;
}

void DjvuView::saveReadingContext()
{
    QVariant item;
    if (layout_->writeReadingHistory(item))
    {
        reading_history_.addItem(item);
    }
}

void DjvuView::back()
{
    if (reading_history_.canGoBack())
    {
        reading_history_.back();
        restore_count_ = 1;

        QVariant item = reading_history_.currentItem();
        ReadingHistoryContext ctx = item.value<ReadingHistoryContext>();
        if (ctx.read_type != read_mode_)
        {
            cur_page_ = ctx.page_number;
            restore_count_++;
            switchLayout(ctx.read_type);
        }
        layout_->restoreByReadingHistory(item);
    }
}

void DjvuView::forward()
{
    if (reading_history_.canGoForward())
    {
        reading_history_.forward();
        restore_count_ = 1;

        QVariant item = reading_history_.currentItem();
        ReadingHistoryContext ctx = item.value<ReadingHistoryContext>();
        if (ctx.read_type != read_mode_)
        {
            cur_page_ = ctx.page_number;
            restore_count_++;
            switchLayout(ctx.read_type);
        }
        layout_->restoreByReadingHistory(item);
    }
}

void DjvuView::openMusicPlayer()
{
    onyx::screen::instance().flush(0, onyx::screen::ScreenProxy::GU);
    sys::SysStatus::instance().requestMusicPlayer(sys::START_PLAYER);
}

void DjvuView::startSlideShow()
{
    status_mgr_.setStatus(ID_SLIDE_SHOW, FUNC_SELECTED);
    sys::SysStatus::instance().enableIdle(false);

    // reset the reading layout and zoom
    zooming(ZOOM_HIDE_MARGIN);
    switchLayout(PAGE_LAYOUT);
    slide_timer_.start(SLIDE_TIME_INTERVAL);

    // enter full screen mode
    emit fullScreen(true);
}

void DjvuView::stopSlideShow()
{
    status_mgr_.setStatus(ID_SLIDE_SHOW, FUNC_NORMAL);
    sys::SysStatus::instance().resetIdle();

    // stop the slide timer
    slide_timer_.stop();

    // exit full screen mode
    emit fullScreen(false);
}

void DjvuView::rotate()
{
    emit rotateScreen();

    RotateDegree degree = getSystemRotateDegree();
    sketch_proxy_.setWidgetOrient( degree );
}

}