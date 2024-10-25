// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/widgets/resizerwidget.h"
#include <QMouseEvent>
#include <QPainter>

namespace widgets {

ResizerWidget::ResizerWidget(QWidget *parent)
	: QWidget(parent)
	, m_originalSize(100, 100)
	, m_targetSize(200, 200)
	, m_bgColor(100, 100, 100)
{
	updateScales();
	center();
}

void ResizerWidget::setBackgroundColor(const QColor &bgColor)
{
	if(bgColor != m_bgColor) {
		m_bgColor = bgColor;
		update();
	}
}

void ResizerWidget::setImage(const QImage &image)
{
	m_originalPixmap = QPixmap::fromImage(image);
	update();
}

void ResizerWidget::setOriginalSize(const QSize &size)
{
	if(size.width() > 0 && size.height() > 0) {
		m_originalSize = size;
		updateScales();
		setOffset(offset());
		update();
	}
}

void ResizerWidget::setTargetSize(const QSize &size)
{
	if(size.width() > 0 && size.height() > 0) {
		m_targetSize = size;
		updateScales();
		setOffset(offset());
		update();
	}
}

void ResizerWidget::setOffset(const QPoint &offset)
{
	if(m_originalSize.width() < m_targetSize.width()) {
		m_offset.rx() = qBound(
			0, offset.x(), m_targetSize.width() - m_originalSize.width());
	} else {
		m_offset.rx() = qBound(
			m_targetSize.width() - m_originalSize.width(), offset.x(), 0);
	}

	if(m_originalSize.height() < m_targetSize.height()) {
		m_offset.ry() = qBound(
			0, offset.y(), m_targetSize.height() - m_originalSize.height());
	} else {
		m_offset.ry() = qBound(
			m_targetSize.height() - m_originalSize.height(), offset.y(), 0);
	}

	update();
	emit offsetChanged(offset);
}

void ResizerWidget::updateScales()
{
	QSize bigScale = m_targetSize.expandedTo(m_originalSize);
	m_scale = qreal(bigScale.scaled(size(), Qt::KeepAspectRatio).width()) /
			  bigScale.width();

	const QSize targetSize = m_targetSize * m_scale;
	m_targetScaled = QRectF(
		QPointF(
			(width() - targetSize.width()) / 2,
			(height() - targetSize.height()) / 2),
		targetSize);

	m_originalScaled = m_originalSize * m_scale;
}

void ResizerWidget::mousePressEvent(QMouseEvent *e)
{
	const QRectF orig(
		m_targetScaled.topLeft() + QPointF(m_offset) * m_scale,
		m_originalScaled);
	if(orig.contains(e->pos())) {
		m_grabPoint = e->pos();
		m_grabOffset = m_offset;
	} else {
		m_grabPoint = QPoint(-1, -1);
	}
}

void ResizerWidget::mouseMoveEvent(QMouseEvent *e)
{
	if(m_grabPoint.x() >= 0) {
		QPoint d = e->pos() - m_grabPoint;
		setOffset(m_grabOffset + d / m_scale);
	}
}

void ResizerWidget::center()
{
	setOffset(QPoint(
		(m_targetSize.width() - m_originalSize.width()) / 2,
		(m_targetSize.height() - m_originalSize.height()) / 2));
}

void ResizerWidget::paintEvent(QPaintEvent *)
{
	QPainter painter(this);

	painter.fillRect(m_targetScaled, m_bgColor);

	QRect original = QRect(
		(m_targetScaled.topLeft() + QPointF(m_offset) * m_scale).toPoint(),
		m_originalScaled);

	if(m_originalPixmap.isNull()) {
		painter.fillRect(original, QColor(200, 200, 200));

	} else {
		painter.drawPixmap(original, m_originalPixmap);
	}

	QColor shade(0, 0, 0, 128);
	painter.fillRect(
		QRect(0, 0, width(), m_targetScaled.y()).intersected(original), shade);
	painter.fillRect(
		QRect(
			0, m_targetScaled.y(), m_targetScaled.x(), m_targetScaled.height())
			.intersected(original),
		shade);
	painter.fillRect(
		QRect(
			m_targetScaled.right(), m_targetScaled.y(), width(),
			m_targetScaled.height())
			.intersected(original),
		shade);
	painter.fillRect(
		QRect(0, m_targetScaled.bottom(), width(), height())
			.intersected(original),
		shade);

	const QRect outline = original & m_targetScaled.toRect();
	painter.setPen(QPen(Qt::black, 1, Qt::SolidLine));
	painter.drawRect(outline);
	painter.setPen(QPen(Qt::white, 1, Qt::DashLine));
	painter.drawRect(outline);
}

void ResizerWidget::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	updateScales();
}

}
