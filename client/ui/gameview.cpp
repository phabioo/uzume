#include "gameview.h"

#include <QKeyEvent>
#include <cmath>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGNode>
#include <QSGTexture>

namespace uzume::ui {

GameView::GameView(QQuickItem* parent) : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  setFlag(ItemIsFocusScope, false);
  setActiveFocusOnTab(true);
  setAcceptedMouseButtons(Qt::LeftButton);
}

GameView::~GameView() {
  if (session_) {
    session_->setCpuConsumer(this, false);
    session_->setViewSize(this, QSize());
  }
}

QSize GameView::physicalSize(const QSizeF& logical, qreal dpr) {
  if (logical.isEmpty() || dpr <= 0) return {};
  return QSize(static_cast<int>(std::ceil(logical.width() * dpr - 1e-6)), static_cast<int>(std::ceil(logical.height() * dpr - 1e-6)));
}

void GameView::reportSize() {
  if (!session_) return;
  const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
  // Needed size = the 1x base frame times the on-screen scale (the source may be several times larger than that
  // and is downscaled to it), so split layouts, where screens are drawn individually, get what they really show.
  const emu::DisplayProfile& prof = session_->displayProfile();
  const QSize base = prof.frameSize();
  qreal scale = 0;  // logical pixels per base pixel
  if (!base.isEmpty()) {
    if (splitDrawing()) {
      for (int i = 0; i < placement_.targets.size() && scale <= 0; ++i) {
        const QRect sr = prof.screenRect(i);
        if (!placement_.targets.at(i).isEmpty() && sr.width() > 0) scale = placement_.targets.at(i).width() / sr.width();
      }
    } else if (!frameRect_.isEmpty()) {
      scale = frameRect_.width() / base.width();
    }
  }
  const QSize px = scale > 0 ? physicalSize(QSizeF(base.width() * scale, base.height() * scale), dpr) : physicalSize(size(), dpr);
  session_->setViewSize(this, px);
}

void GameView::itemChange(ItemChange change, const ItemChangeData& value) {
  QQuickItem::itemChange(change, value);
  if (change == ItemDevicePixelRatioHasChanged || change == ItemSceneChange) reportSize();
}

void GameView::setSession(GameSession* s) {
  if (session_ == s) {
    return;
  }
  if (session_) {
    session_->disconnect(this);
    session_->setCpuConsumer(this, false);
    session_->setViewSize(this, QSize());
  }
  session_ = s;
  frame_ = QImage();
  lastFrameSize_ = QSize();
  if (session_) {
    connect(session_, &GameSession::frameChanged, this, &GameView::onFrame);
    connect(session_, &GameSession::displayStateChanged, this, [this]() { update(); });  // texture <-> readback image
    session_->setCpuConsumer(this, cpuOnly_);
    frame_ = session_->frame();
  }
  frameDirty_ = true;
  updateFrameRect();
  update();
  emit sessionChanged();
}

void GameView::setIntegerScale(bool on) {
  if (integerScale_ == on) {
    return;
  }
  integerScale_ = on;
  updateFrameRect();
  update();
  emit integerScaleChanged();
}

void GameView::setStrictInteger(bool on) {
  if (strictInteger_ == on) {
    return;
  }
  strictInteger_ = on;
  frameDirty_ = true;
  updateFrameRect();
  update();
  emit pictureOptionsChanged();
}

void GameView::setAspectRatio(const QString& value) {
  const AspectMode mode = aspectModeFromString(value);
  const QString next = mode == AspectMode::Ratio43 ? QStringLiteral("4:3") : mode == AspectMode::Stretch ? QStringLiteral("stretch") : QStringLiteral("core");
  if (aspectRatio_ == next) {
    return;
  }
  aspectRatio_ = next;
  aspect_ = mode;
  frameDirty_ = true;
  updateFrameRect();
  update();
  emit pictureOptionsChanged();
}

void GameView::setLayout(const QString& layout) {
  const QString next = isScreenLayout(layout) ? layout : QString::fromLatin1(kLayoutStacked);
  if (layout_ == next) {
    return;
  }
  layout_ = next;
  frameDirty_ = true;
  updateFrameRect();
  update();
  emit layoutChanged();
}

void GameView::setSwap(bool on) {
  if (swap_ == on) {
    return;
  }
  swap_ = on;
  frameDirty_ = true;
  updateFrameRect();
  update();
  emit layoutChanged();
}

void GameView::setCpuOnly(bool on) {
  if (cpuOnly_ == on) {
    return;
  }
  cpuOnly_ = on;
  if (session_) session_->setCpuConsumer(this, on);
  frameDirty_ = true;
  update();
  emit cpuOnlyChanged();
}

// "Stacked" with a vertical source is the frame as the core delivers it (one texture, the fast and unchanged path).
bool GameView::splitDrawing() const {
  if (!session_ || session_->screenCount() < 2) {
    return false;
  }
  return layout_ != QLatin1String(kLayoutStacked) || (swap_ && layoutSupportsSwap(layout_)) || session_->displayProfile().layout != QLatin1String("vertical") ||
         aspect_ == AspectMode::Ratio43;
}

void GameView::onFrame() {
  if (!session_) {
    return;
  }
  const QSize before = frame_.size();
  frame_ = session_->frame();
  frameDirty_ = true;
  if (frame_.size() != before) {
    updateFrameRect();
  }
  update();
}

void GameView::updateFrameRect() {
  // Fit the 1x base size when the system declares one, not the delivered frame: that one follows the readback limit,
  // which follows this rect (integer scaling would feed back: frame size -> rect -> limit -> frame size).
  QSizeF fs = session_ ? QSizeF(session_->displayProfile().frameSize()) : QSizeF();
  if (fs.isEmpty()) {
    // No base size from the system: the delivered frame; after the session dropped it (zero-copy active) the last one.
    if (!frame_.isNull()) lastFrameSize_ = frame_.size();
    fs = lastFrameSize_;
  }
  QRectF r;
  if (splitDrawing()) {
    placement_ = placeScreens(session_->displayProfile(), layout_, size(), integerScale_, strictInteger_, aspect_, swap_);
    r = placement_.content;
  } else {
    r = fitPicture(fs, size(), integerScale_, strictInteger_, aspect_);
    placement_ = ScreenPlacement{};
    if (session_) {
      const emu::DisplayProfile& prof = session_->displayProfile();
      for (int i = 0; i < prof.screens.size(); ++i) {
        const QRect sr = prof.screenRect(i);
        const QSize base = prof.frameSize();
        if (base.isEmpty() || r.isEmpty()) {
          placement_.targets.append(QRectF());
          continue;
        }
        const qreal sx = r.width() / base.width(), sy = r.height() / base.height();
        placement_.targets.append(QRectF(r.x() + sr.x() * sx, r.y() + sr.y() * sy, sr.width() * sx, sr.height() * sy));
      }
      placement_.content = r;
    }
  }
  if (r != frameRect_) {
    frameRect_ = r;
    emit frameRectChanged();
  }
  reportSize();
}

void GameView::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
  QQuickItem::geometryChange(newGeometry, oldGeometry);
  updateFrameRect();
  update();
}

namespace {
// A texture drawn smaller than it is (physical pixels) must be filtered, or fine detail aliases and shimmers on
// moving content (mipmapped linear = box-ish average). Drawn at 1:1 or larger, nearest keeps the pixels crisp.
bool isDownscaled(const QSize& tex, const QRectF& target, qreal dpr) {
  return tex.width() > std::ceil(target.width() * dpr - 1e-6) || tex.height() > std::ceil(target.height() * dpr - 1e-6);
}

void applyFiltering(QSGImageNode* n, bool down) {
  n->setFiltering(down ? QSGTexture::Linear : QSGTexture::Nearest);
  n->setMipmapFiltering(down ? QSGTexture::Linear : QSGTexture::None);
}
}  // namespace

namespace {
// Node tree of the zero-copy split layout: one image node per placement target (null = nothing drawn there), all on the
// same GPU texture. Kept between frames; only rects and textures are updated.
struct GpuGroupNode : QSGNode {
  QList<QSGImageNode*> screens;
};

// The ownership flag belongs to the texture the node holds at the moment of the call (setTexture() deletes an owned
// old one), so it is set for the new texture afterwards.
void setNodeTexture(QSGImageNode* n, QSGTexture* tex, bool owned) {
  n->setTexture(tex);
  n->setOwnsTexture(owned);
}

}  // namespace

QSGNode* GameView::paintGpu(QSGNode* old, QSGTexture* tex, const QSize& content, qreal dpr) {
  if (splitDrawing()) {
    auto* group = dynamic_cast<GpuGroupNode*>(old);
    if (group == nullptr) {
      delete old;
      group = new GpuGroupNode;
    }
    const emu::DisplayProfile& prof = session_->displayProfile();
    group->screens.resize(placement_.targets.size(), nullptr);
    for (int i = 0; i < placement_.targets.size(); ++i) {
      const QRectF target = placement_.targets.at(i);
      const QRectF src = screenSourceRect(prof, content, i);
      QSGImageNode*& n = group->screens[i];
      if (target.isEmpty() || src.isEmpty()) {
        if (n) {
          group->removeChildNode(n);
          delete n;
          n = nullptr;
        }
        continue;
      }
      if (n == nullptr) {
        n = window()->createImageNode();
        group->appendChildNode(n);
      }
      const bool down = isDownscaled(src.size().toSize(), target, dpr);
      setNodeTexture(n, tex, false);  // first: the old texture may already be gone, no other call may touch it
      n->setFiltering(down ? QSGTexture::Linear : QSGTexture::Nearest);
      n->setMipmapFiltering(QSGTexture::None);
      n->setSourceRect(src);  // texture pixels; the image sits in the top-left corner of a possibly larger texture
      n->setRect(target);
    }
    return group;
  }
  auto* node = dynamic_cast<QSGImageNode*>(old);
  if (node == nullptr) {
    delete old;
    node = window()->createImageNode();
  }
  const bool down = isDownscaled(content, frameRect_, dpr);
  setNodeTexture(node, tex, false);  // first: the old texture may already be gone, no other call may touch it
  node->setFiltering(down ? QSGTexture::Linear : QSGTexture::Nearest);
  node->setMipmapFiltering(QSGTexture::None);
  node->setSourceRect(QRectF(QPointF(0, 0), QSizeF(content)));
  node->setRect(frameRect_);
  return node;
}

QSGNode* GameView::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
  if (window() == nullptr || frameRect_.isEmpty()) {
    delete old;
    lastGpu_ = false;
    return nullptr;
  }
  const qreal dpr = window()->effectiveDevicePixelRatio();
  // Zero-copy display (ADR 0027): the game's GPU texture, when its source has one for this window now.
  QSGTexture* gpu = nullptr;
  QSize content;
  if (session_) {
    if (DisplayTextureSource* src = cpuOnly_ ? nullptr : session_->displaySource()) {
      gpu = src->texture(window(), &content);
      if (gpu && (content.isEmpty() || gpu->textureSize().isEmpty())) gpu = nullptr;
    }
  }
  if (gpu) {
    QSGNode* n = paintGpu(old, gpu, content, dpr);
    lastGpu_ = true;
    session_->confirmDisplayShown();  // the texture is in the tree: the readback may stop
    return n;
  }
  if (frame_.isNull()) {
    delete old;
    lastGpu_ = false;
    return nullptr;
  }
  const bool wasGpu = lastGpu_;
  lastGpu_ = false;
  if (splitDrawing()) {
    // One image node per visible screen, each with its own texture cut out of the frame (a few 100 KB per frame).
    delete old;
    auto* group = new QSGNode;
    const emu::DisplayProfile& prof = session_->displayProfile();
    for (int i = 0; i < placement_.targets.size(); ++i) {
      const QRectF target = placement_.targets.at(i);
      if (target.isEmpty()) {
        continue;
      }
      const QRectF src = screenSourceRect(prof, frame_.size(), i);
      if (src.isEmpty()) {
        continue;
      }
      auto* n = window()->createImageNode();
      const QImage part = frame_.copy(src.toRect());
      const bool down = isDownscaled(part.size(), target, dpr);
      applyFiltering(n, down);
      n->setOwnsTexture(true);
      n->setTexture(window()->createTextureFromImage(part, down ? QQuickWindow::TextureHasMipmaps : QQuickWindow::CreateTextureOptions()));
      n->setRect(target);
      group->appendChildNode(n);
    }
    frameDirty_ = false;
    return group;
  }
  auto* node = dynamic_cast<QSGImageNode*>(old);
  if (node == nullptr && old != nullptr) {
    delete old;  // the previous tree was a split group
  }
  if (node == nullptr) {
    node = window()->createImageNode();
    node->setOwnsTexture(true);
    frameDirty_ = true;
  }
  const bool down = isDownscaled(frame_.size(), frameRect_, dpr);
  if (frameDirty_ || down != lastDown_ || wasGpu) {
    setNodeTexture(node, window()->createTextureFromImage(frame_, down ? QQuickWindow::TextureHasMipmaps : QQuickWindow::CreateTextureOptions()), true);
    applyFiltering(node, down);
    node->setSourceRect(QRectF());  // the whole texture again
    lastDown_ = down;
    frameDirty_ = false;
  }
  node->setRect(frameRect_);
  return node;
}

void GameView::keyPressEvent(QKeyEvent* e) {
  if (e->isAutoRepeat()) {
    e->accept();
    return;
  }
  if (e->key() == Qt::Key_Escape && (e->modifiers() & Qt::ShiftModifier)) {
    e->ignore();  // Shift+Esc (pause for everyone) is handled by the game screen
    return;
  }
  if (e->key() == Qt::Key_Escape) {
    emit escapePressed();
    e->accept();
    return;
  }
  if (session_ && session_->keyEvent(e->key(), true, e->modifiers())) {
    e->accept();
    return;
  }
  e->ignore();
}

void GameView::keyReleaseEvent(QKeyEvent* e) {
  if (e->isAutoRepeat()) {
    e->accept();
    return;
  }
  if (session_ && session_->keyEvent(e->key(), false, e->modifiers())) {
    e->accept();
    return;
  }
  e->ignore();
}

void GameView::focusOutEvent(QFocusEvent* e) {
  QQuickItem::focusOutEvent(e);
  if (session_) {
    session_->releaseAllKeys();
  }
  touching_ = false;
}

std::optional<QPointF> GameView::touchTarget(const QPointF& itemPos, bool clamp) const {
  if (!session_) {
    return std::nullopt;
  }
  return splitDrawing() ? touchToFrameFor(session_->displayProfile(), placement_, itemPos, clamp)
                        : touchToFrame(session_->displayProfile(), frameRect_, itemPos, clamp);
}

void GameView::touch(const QPointF& pos, bool press, bool clamp) {
  if (!session_) {
    return;
  }
  const auto p = touchTarget(pos, clamp);
  if (!p) {
    return;
  }
  touching_ = press;
  session_->setPointer(*p, press);
}

void GameView::mousePressEvent(QMouseEvent* e) {
  forceActiveFocus();
  touching_ = false;
  touch(e->position(), true, false);
  e->setAccepted(true);
}

void GameView::mouseMoveEvent(QMouseEvent* e) {
  if (touching_) {
    touch(e->position(), true, true);
  }
  e->setAccepted(true);
}

void GameView::mouseReleaseEvent(QMouseEvent* e) {
  if (touching_ && session_) {
    const auto p = touchTarget(e->position(), true);
    if (p) {
      session_->setPointer(*p, false);
    }
  }
  touching_ = false;
  e->setAccepted(true);
}

void GameView::mouseUngrabEvent() {
  if (touching_ && session_) {
    session_->setPointer(QPointF(0, 0), false);
  }
  touching_ = false;
}

}  // namespace uzume::ui
