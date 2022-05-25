/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "RenderFlexibleBox.h"

#include "LayoutRepainter.h"
#include "RenderView.h"

#if ENABLE(CSS3_FLEXBOX)

namespace WebCore {

// Normally, -1 and 0 are not valid in a HashSet, but these are relatively likely flex-order values. Instead,
// we make the two smallest int values invalid flex-order values (in the css parser code we clamp them to
// int min + 2).
struct FlexOrderHashTraits : WTF::GenericHashTraits<int> {
    static const bool emptyValueIsZero = false;
    static int emptyValue() { return std::numeric_limits<int>::min(); }
    static void constructDeletedValue(int& slot) { slot = std::numeric_limits<int>::min() + 1; }
    static bool isDeletedValue(int value) { return value == std::numeric_limits<int>::min() + 1; }
};

typedef HashSet<int, DefaultHash<int>::Hash, FlexOrderHashTraits> FlexOrderHashSet;

class RenderFlexibleBox::TreeOrderIterator {
public:
    explicit TreeOrderIterator(RenderFlexibleBox* flexibleBox)
        : m_flexibleBox(flexibleBox)
        , m_currentChild(0)
    {
    }

    RenderBox* first()
    {
        reset();
        return next();
    }

    RenderBox* next()
    {
        RenderObject* child = m_currentChild ? m_currentChild->nextSibling() : m_flexibleBox->firstChild();
        // FIXME: Inline nodes (like <img> or <input>) should also be treated as boxes.
        while (child && !child->isBox())
            child = child->nextSibling();

        if (child)
            m_flexOrderValues.add(child->style()->flexOrder());

        m_currentChild = toRenderBox(child);
        return m_currentChild;
    }

    void reset()
    {
        m_currentChild = 0;
    }

    const FlexOrderHashSet& flexOrderValues()
    {
        return m_flexOrderValues;
    }

private:
    RenderFlexibleBox* m_flexibleBox;
    RenderBox* m_currentChild;
    FlexOrderHashSet m_flexOrderValues;
};

class RenderFlexibleBox::FlexOrderIterator {
public:
    FlexOrderIterator(RenderFlexibleBox* flexibleBox, const FlexOrderHashSet& flexOrderValues)
        : m_flexibleBox(flexibleBox)
        , m_currentChild(0)
        , m_orderValuesIterator(0)
    {
        copyToVector(flexOrderValues, m_orderValues);
        std::sort(m_orderValues.begin(), m_orderValues.end());
    }

    RenderBox* first()
    {
        reset();
        return next();
    }

    RenderBox* next()
    {
        RenderObject* child = m_currentChild;
        do {
            if (!child) {
                if (m_orderValuesIterator == m_orderValues.end())
                    return 0;
                if (m_orderValuesIterator) {
                    ++m_orderValuesIterator;
                    if (m_orderValuesIterator == m_orderValues.end())
                        return 0;
                } else
                    m_orderValuesIterator = m_orderValues.begin();

                child = m_flexibleBox->firstChild();
            } else
                child = child->nextSibling();
        } while (!child || !child->isBox() || child->style()->flexOrder() != *m_orderValuesIterator);

        m_currentChild = toRenderBox(child);
        return m_currentChild;
    }

    void reset()
    {
        m_currentChild = 0;
        m_orderValuesIterator = 0;
    }

private:
    RenderFlexibleBox* m_flexibleBox;
    RenderBox* m_currentChild;
    Vector<int> m_orderValues;
    Vector<int>::const_iterator m_orderValuesIterator;
};


RenderFlexibleBox::RenderFlexibleBox(Node* node)
    : RenderBlock(node)
{
}

RenderFlexibleBox::~RenderFlexibleBox()
{
}

const char* RenderFlexibleBox::renderName() const
{
    return "RenderFlexibleBox";
}

void RenderFlexibleBox::layoutBlock(bool relayoutChildren, int, BlockLayoutPass)
{
    ASSERT(needsLayout());

    if (!relayoutChildren && simplifiedLayout())
        return;

    LayoutRepainter repainter(*this, checkForRepaintDuringLayout());
    LayoutStateMaintainer statePusher(view(), this, IntSize(x(), y()), hasTransform() || hasReflection() || style()->isFlippedBlocksWritingMode());

    IntSize previousSize = size();

    // FIXME: In theory we should only have to call one of these.
    // computeLogicalWidth for flex-flow:row and computeLogicalHeight for flex-flow:column.
    computeLogicalWidth();
    computeLogicalHeight();

    m_overflow.clear();

    layoutInlineDirection(relayoutChildren);

    if (isColumnFlow())
        computeLogicalWidth();
    else
        computeLogicalHeight();

    if (size() != previousSize)
        relayoutChildren = true;

    layoutPositionedObjects(relayoutChildren || isRoot());

    statePusher.pop();

    updateLayerTransform();

    repainter.repaintAfterLayout();

    setNeedsLayout(false);
}

bool RenderFlexibleBox::hasOrthogonalFlow(RenderBox* child) const
{
    // FIXME: If the child is a flexbox, then we need to check isHorizontalFlow.
    return isHorizontalFlow() != child->isHorizontalWritingMode();
}

bool RenderFlexibleBox::isColumnFlow() const
{
    EFlexFlow flow = style()->flexFlow();
    return flow == FlowColumn || flow == FlowColumnReverse;
}

bool RenderFlexibleBox::isHorizontalFlow() const
{
    if (isHorizontalWritingMode())
        return !isColumnFlow();
    return isColumnFlow();
}

bool RenderFlexibleBox::isLeftToRightFlow() const
{
    if (isColumnFlow())
        return style()->writingMode() == TopToBottomWritingMode || style()->writingMode() == LeftToRightWritingMode;
    return style()->isLeftToRightDirection();
}

bool RenderFlexibleBox::isFlowAwareLogicalHeightAuto() const
{
    Length height = isHorizontalFlow() ? style()->height() : style()->width();
    return height.isAuto();
}

void RenderFlexibleBox::setFlowAwareLogicalHeight(LayoutUnit size)
{
    if (isHorizontalFlow())
        setHeight(size);
    else
        setWidth(size);
}

LayoutUnit RenderFlexibleBox::flowAwareLogicalHeightForChild(RenderBox* child)
{
    return isHorizontalFlow() ? child->height() : child->width();
}

LayoutUnit RenderFlexibleBox::flowAwareLogicalWidthForChild(RenderBox* child)
{
    return isHorizontalFlow() ? child->width() : child->height();
}

LayoutUnit RenderFlexibleBox::flowAwareLogicalHeight() const
{
    return isHorizontalFlow() ? height() : width();
}

LayoutUnit RenderFlexibleBox::flowAwareLogicalWidth() const
{
    return isHorizontalFlow() ? width() : height();
}

LayoutUnit RenderFlexibleBox::flowAwareContentLogicalHeight() const
{
    return isHorizontalFlow() ? contentHeight() : contentWidth();
}

LayoutUnit RenderFlexibleBox::flowAwareContentLogicalWidth() const
{
    return isHorizontalFlow() ? contentWidth() : contentHeight();
}

WritingMode RenderFlexibleBox::transformedWritingMode() const
{
    WritingMode mode = style()->writingMode();
    if (!isColumnFlow())
        return mode;

    switch (mode) {
    case TopToBottomWritingMode:
    case BottomToTopWritingMode:
        return style()->isLeftToRightDirection() ? LeftToRightWritingMode : RightToLeftWritingMode;
    case LeftToRightWritingMode:
    case RightToLeftWritingMode:
        return style()->isLeftToRightDirection() ? TopToBottomWritingMode : BottomToTopWritingMode;
    }
    ASSERT_NOT_REACHED();
    return TopToBottomWritingMode;
}

LayoutUnit RenderFlexibleBox::flowAwareBorderStart() const
{
    if (isHorizontalFlow())
        return isLeftToRightFlow() ? borderLeft() : borderRight();
    return isLeftToRightFlow() ? borderTop() : borderBottom();
}

LayoutUnit RenderFlexibleBox::flowAwareBorderBefore() const
{
    switch (transformedWritingMode()) {
    case TopToBottomWritingMode:
        return borderTop();
    case BottomToTopWritingMode:
        return borderBottom();
    case LeftToRightWritingMode:
        return borderLeft();
    case RightToLeftWritingMode:
        return borderRight();
    }
    ASSERT_NOT_REACHED();
    return borderTop();
}

LayoutUnit RenderFlexibleBox::flowAwareBorderAfter() const
{
    switch (transformedWritingMode()) {
    case TopToBottomWritingMode:
        return borderBottom();
    case BottomToTopWritingMode:
        return borderTop();
    case LeftToRightWritingMode:
        return borderRight();
    case RightToLeftWritingMode:
        return borderLeft();
    }
    ASSERT_NOT_REACHED();
    return borderBottom();
}

LayoutUnit RenderFlexibleBox::flowAwareBorderAndPaddingLogicalHeight() const
{
    return isHorizontalFlow() ? borderAndPaddingHeight() : borderAndPaddingWidth();
}

LayoutUnit RenderFlexibleBox::flowAwarePaddingStart() const
{
    if (isHorizontalFlow())
        return isLeftToRightFlow() ? paddingLeft() : paddingRight();
    return isLeftToRightFlow() ? paddingTop() : paddingBottom();
}

LayoutUnit RenderFlexibleBox::flowAwarePaddingBefore() const
{
    switch (transformedWritingMode()) {
    case TopToBottomWritingMode:
        return paddingTop();
    case BottomToTopWritingMode:
        return paddingBottom();
    case LeftToRightWritingMode:
        return paddingLeft();
    case RightToLeftWritingMode:
        return paddingRight();
    }
    ASSERT_NOT_REACHED();
    return paddingTop();
}

LayoutUnit RenderFlexibleBox::flowAwarePaddingAfter() const
{
    switch (transformedWritingMode()) {
    case TopToBottomWritingMode:
        return paddingBottom();
    case BottomToTopWritingMode:
        return paddingTop();
    case LeftToRightWritingMode:
        return paddingRight();
    case RightToLeftWritingMode:
        return paddingLeft();
    }
    ASSERT_NOT_REACHED();
    return paddingBottom();
}

LayoutUnit RenderFlexibleBox::flowAwareMarginStartForChild(RenderBox* child) const
{
    if (isHorizontalFlow())
        return isLeftToRightFlow() ? child->marginLeft() : child->marginRight();
    return isLeftToRightFlow() ? child->marginTop() : child->marginBottom();
}

LayoutUnit RenderFlexibleBox::flowAwareMarginEndForChild(RenderBox* child) const
{
    if (isHorizontalFlow())
        return isLeftToRightFlow() ? child->marginRight() : child->marginLeft();
    return isLeftToRightFlow() ? child->marginBottom() : child->marginTop();
}

LayoutUnit RenderFlexibleBox::flowAwareMarginBeforeForChild(RenderBox* child) const
{
    switch (transformedWritingMode()) {
    case TopToBottomWritingMode:
        return child->marginTop();
    case BottomToTopWritingMode:
        return child->marginBottom();
    case LeftToRightWritingMode:
        return child->marginLeft();
    case RightToLeftWritingMode:
        return child->marginRight();
    }
    ASSERT_NOT_REACHED();
    return marginTop();
}

LayoutUnit RenderFlexibleBox::flowAwareMarginAfterForChild(RenderBox* child) const
{
    switch (transformedWritingMode()) {
    case TopToBottomWritingMode:
        return child->marginBottom();
    case BottomToTopWritingMode:
        return child->marginTop();
    case LeftToRightWritingMode:
        return child->marginRight();
    case RightToLeftWritingMode:
        return child->marginLeft();
    }
    ASSERT_NOT_REACHED();
    return marginBottom();
}

LayoutUnit RenderFlexibleBox::flowAwareMarginLogicalHeightForChild(RenderBox* child) const
{
    return isHorizontalFlow() ? child->marginTop() + child->marginBottom() : child->marginLeft() + child->marginRight();
}

LayoutPoint RenderFlexibleBox::flowAwareLogicalLocationForChild(RenderBox* child) const
{
    return isHorizontalFlow() ? child->location() : child->location().transposedPoint();
}

void RenderFlexibleBox::setFlowAwareMarginStartForChild(RenderBox* child, LayoutUnit margin)
{
    if (isHorizontalFlow()) {
        if (isLeftToRightFlow())
            child->setMarginLeft(margin);
        else
            child->setMarginRight(margin);
    } else {
        if (isLeftToRightFlow())
            child->setMarginTop(margin);
        else
            child->setMarginBottom(margin);
    }
}

void RenderFlexibleBox::setFlowAwareMarginEndForChild(RenderBox* child, LayoutUnit margin)
{
    if (isHorizontalFlow()) {
        if (isLeftToRightFlow())
            child->setMarginRight(margin);
        else
            child->setMarginLeft(margin);
    } else {
        if (isLeftToRightFlow())
            child->setMarginBottom(margin);
        else
            child->setMarginTop(margin);
    }
}

void RenderFlexibleBox::setFlowAwareLogicalLocationForChild(RenderBox* child, const LayoutPoint& location)
{
    if (isHorizontalFlow())
        child->setLocation(location);
    else
        child->setLocation(location.transposedPoint());
}

LayoutUnit RenderFlexibleBox::logicalBorderAndPaddingWidthForChild(RenderBox* child) const
{
    return isHorizontalFlow() ? child->borderAndPaddingWidth() : child->borderAndPaddingHeight();
}

LayoutUnit RenderFlexibleBox::logicalScrollbarHeightForChild(RenderBox* child) const
{
    return isHorizontalFlow() ? child->verticalScrollbarWidth() : child->horizontalScrollbarHeight();
}

Length RenderFlexibleBox::marginStartStyleForChild(RenderBox* child) const
{
    if (isHorizontalFlow())
        return isLeftToRightFlow() ? child->style()->marginLeft() : child->style()->marginRight();
    return isLeftToRightFlow() ? child->style()->marginTop() : child->style()->marginBottom();
}

Length RenderFlexibleBox::marginEndStyleForChild(RenderBox* child) const
{
    if (isHorizontalFlow())
        return isLeftToRightFlow() ? child->style()->marginRight() : child->style()->marginLeft();
    return isLeftToRightFlow() ? child->style()->marginBottom() : child->style()->marginTop();
}

LayoutUnit RenderFlexibleBox::preferredLogicalContentWidthForFlexItem(RenderBox* child) const
{
    Length width = isHorizontalFlow() ? child->style()->width() : child->style()->height();
    if (width.isAuto()) {
        LayoutUnit logicalWidth = hasOrthogonalFlow(child) ? child->logicalHeight() : child->maxPreferredLogicalWidth();
        return logicalWidth - logicalBorderAndPaddingWidthForChild(child) - logicalScrollbarHeightForChild(child);
    }
    return isHorizontalFlow() ? child->contentWidth() : child->contentHeight();
}

void RenderFlexibleBox::layoutInlineDirection(bool relayoutChildren)
{
    LayoutUnit preferredLogicalWidth;
    float totalPositiveFlexibility;
    float totalNegativeFlexibility;
    TreeOrderIterator treeIterator(this);

    computePreferredLogicalWidth(relayoutChildren, treeIterator, preferredLogicalWidth, totalPositiveFlexibility, totalNegativeFlexibility);
    LayoutUnit availableFreeSpace = flowAwareContentLogicalWidth() - preferredLogicalWidth;

    FlexOrderIterator flexIterator(this, treeIterator.flexOrderValues());
    InflexibleFlexItemSize inflexibleItems;
    WTF::Vector<LayoutUnit> childSizes;
    while (!runFreeSpaceAllocationAlgorithmInlineDirection(flexIterator, availableFreeSpace, totalPositiveFlexibility, totalNegativeFlexibility, inflexibleItems, childSizes)) {
        ASSERT(totalPositiveFlexibility >= 0 && totalNegativeFlexibility >= 0);
        ASSERT(inflexibleItems.size() > 0);
    }

    layoutAndPlaceChildrenInlineDirection(flexIterator, childSizes, availableFreeSpace, totalPositiveFlexibility);
}

float RenderFlexibleBox::logicalPositiveFlexForChild(RenderBox* child) const
{
    return isHorizontalFlow() ? child->style()->flexboxWidthPositiveFlex() : child->style()->flexboxHeightPositiveFlex();
}

float RenderFlexibleBox::logicalNegativeFlexForChild(RenderBox* child) const
{
    return isHorizontalFlow() ? child->style()->flexboxWidthNegativeFlex() : child->style()->flexboxHeightNegativeFlex();
}

LayoutUnit RenderFlexibleBox::availableLogicalHeightForChild(RenderBox* child)
{
    LayoutUnit contentLogicalHeight = flowAwareContentLogicalHeight();
    LayoutUnit currentChildHeight = flowAwareMarginLogicalHeightForChild(child) + flowAwareLogicalHeightForChild(child);
    return contentLogicalHeight - currentChildHeight;
}

LayoutUnit RenderFlexibleBox::marginBoxAscent(RenderBox* child)
{
    LayoutUnit ascent = child->firstLineBoxBaseline();
    if (ascent == -1)
        ascent = flowAwareLogicalHeightForChild(child) + flowAwareMarginAfterForChild(child);
    return ascent + flowAwareMarginBeforeForChild(child);
}

void RenderFlexibleBox::computePreferredLogicalWidth(bool relayoutChildren, TreeOrderIterator& iterator, LayoutUnit& preferredLogicalWidth, float& totalPositiveFlexibility, float& totalNegativeFlexibility)
{
    preferredLogicalWidth = 0;
    totalPositiveFlexibility = totalNegativeFlexibility = 0;

    LayoutUnit flexboxAvailableLogicalWidth = flowAwareContentLogicalWidth();
    for (RenderBox* child = iterator.first(); child; child = iterator.next()) {
        // We always have to lay out flexible objects again, since the flex distribution
        // may have changed, and we need to reallocate space.
        child->clearOverrideSize();
        if (!relayoutChildren)
            child->setChildNeedsLayout(true);
        child->layoutIfNeeded();

        // We can't just use marginStartForChild, et. al. because "auto" needs to be treated as 0.
        if (isHorizontalFlow()) {
            preferredLogicalWidth += child->style()->marginLeft().calcMinValue(flexboxAvailableLogicalWidth);
            preferredLogicalWidth += child->style()->marginRight().calcMinValue(flexboxAvailableLogicalWidth);
        } else {
            preferredLogicalWidth += child->style()->marginTop().calcMinValue(flexboxAvailableLogicalWidth);
            preferredLogicalWidth += child->style()->marginBottom().calcMinValue(flexboxAvailableLogicalWidth);
        }

        preferredLogicalWidth += logicalBorderAndPaddingWidthForChild(child);
        preferredLogicalWidth += preferredLogicalContentWidthForFlexItem(child);

        totalPositiveFlexibility += logicalPositiveFlexForChild(child);
        totalNegativeFlexibility += logicalNegativeFlexForChild(child);
    }
}

// Returns true if we successfully ran the algorithm and sized the flex items.
bool RenderFlexibleBox::runFreeSpaceAllocationAlgorithmInlineDirection(FlexOrderIterator& iterator, LayoutUnit& availableFreeSpace, float& totalPositiveFlexibility, float& totalNegativeFlexibility, InflexibleFlexItemSize& inflexibleItems, WTF::Vector<LayoutUnit>& childSizes)
{
    childSizes.clear();

    LayoutUnit flexboxAvailableLogicalWidth = flowAwareContentLogicalWidth();
    for (RenderBox* child = iterator.first(); child; child = iterator.next()) {
        LayoutUnit childPreferredSize;
        if (inflexibleItems.contains(child))
            childPreferredSize = inflexibleItems.get(child);
        else {
            childPreferredSize = preferredLogicalContentWidthForFlexItem(child);
            if (availableFreeSpace > 0 && totalPositiveFlexibility > 0) {
                childPreferredSize += lroundf(availableFreeSpace * logicalPositiveFlexForChild(child) / totalPositiveFlexibility);

                Length childLogicalMaxWidth = isHorizontalFlow() ? child->style()->maxWidth() : child->style()->maxHeight();
                if (!childLogicalMaxWidth.isUndefined() && childLogicalMaxWidth.isSpecified() && childPreferredSize > childLogicalMaxWidth.calcValue(flexboxAvailableLogicalWidth)) {
                    childPreferredSize = childLogicalMaxWidth.calcValue(flexboxAvailableLogicalWidth);
                    availableFreeSpace -= childPreferredSize - preferredLogicalContentWidthForFlexItem(child);
                    totalPositiveFlexibility -= logicalPositiveFlexForChild(child);

                    inflexibleItems.set(child, childPreferredSize);
                    return false;
                }
            } else if (availableFreeSpace < 0 && totalNegativeFlexibility > 0) {
                childPreferredSize += lroundf(availableFreeSpace * logicalNegativeFlexForChild(child) / totalNegativeFlexibility);

                Length childLogicalMinWidth = isHorizontalFlow() ? child->style()->minWidth() : child->style()->minHeight();
                if (!childLogicalMinWidth.isUndefined() && childLogicalMinWidth.isSpecified() && childPreferredSize < childLogicalMinWidth.calcValue(flexboxAvailableLogicalWidth)) {
                    childPreferredSize = childLogicalMinWidth.calcValue(flexboxAvailableLogicalWidth);
                    availableFreeSpace += preferredLogicalContentWidthForFlexItem(child) - childPreferredSize;
                    totalNegativeFlexibility -= logicalNegativeFlexForChild(child);

                    inflexibleItems.set(child, childPreferredSize);
                    return false;
                }
            }
        }
        childSizes.append(childPreferredSize);
    }
    return true;
}

static bool hasPackingSpace(LayoutUnit availableFreeSpace, float totalPositiveFlexibility)
{
    return availableFreeSpace > 0 && !totalPositiveFlexibility;
}

void RenderFlexibleBox::setLogicalOverrideSize(RenderBox* child, LayoutUnit childPreferredSize)
{
    // FIXME: Rename setOverrideWidth/setOverrideHeight to setOverrideLogicalWidth/setOverrideLogicalHeight.
    if (hasOrthogonalFlow(child))
        child->setOverrideHeight(childPreferredSize);
    else
        child->setOverrideWidth(childPreferredSize);
}

void RenderFlexibleBox::layoutAndPlaceChildrenInlineDirection(FlexOrderIterator& iterator, const WTF::Vector<LayoutUnit>& childSizes, LayoutUnit availableFreeSpace, float totalPositiveFlexibility)
{
    LayoutUnit startEdge = flowAwareBorderStart() + flowAwarePaddingStart();

    if (hasPackingSpace(availableFreeSpace, totalPositiveFlexibility)) {
        if (style()->flexPack() == PackEnd)
            startEdge += availableFreeSpace;
        else if (style()->flexPack() == PackCenter)
            startEdge += availableFreeSpace / 2;
    }

    LayoutUnit logicalTop = flowAwareBorderBefore() + flowAwarePaddingBefore();
    LayoutUnit totalLogicalWidth = flowAwareLogicalWidth();
    if (isFlowAwareLogicalHeightAuto())
        setFlowAwareLogicalHeight(0);
    LayoutUnit maxAscent = 0, maxDescent = 0; // Used when flex-align: baseline.
    size_t i = 0;
    for (RenderBox* child = iterator.first(); child; child = iterator.next(), ++i) {
        LayoutUnit childPreferredSize = childSizes[i] + logicalBorderAndPaddingWidthForChild(child);
        setLogicalOverrideSize(child, childPreferredSize);
        child->setChildNeedsLayout(true);
        child->layoutIfNeeded();

        if (child->style()->flexAlign() == AlignBaseline) {
            LayoutUnit ascent = marginBoxAscent(child);
            LayoutUnit descent = (flowAwareMarginLogicalHeightForChild(child) + flowAwareLogicalHeightForChild(child)) - ascent;

            maxAscent = std::max(maxAscent, ascent);
            maxDescent = std::max(maxDescent, descent);

            // FIXME: add flowAwareScrollbarLogicalHeight.
            if (isFlowAwareLogicalHeightAuto())
                setFlowAwareLogicalHeight(std::max(flowAwareLogicalHeight(), flowAwareBorderAndPaddingLogicalHeight() + flowAwareMarginLogicalHeightForChild(child) + maxAscent + maxDescent + scrollbarLogicalHeight()));
        } else if (isFlowAwareLogicalHeightAuto())
            setFlowAwareLogicalHeight(std::max(flowAwareLogicalHeight(), flowAwareBorderAndPaddingLogicalHeight() + flowAwareMarginLogicalHeightForChild(child) + flowAwareLogicalHeightForChild(child) + scrollbarLogicalHeight()));

        if (marginStartStyleForChild(child).isAuto())
            setFlowAwareMarginStartForChild(child, 0);
        if (marginEndStyleForChild(child).isAuto())
            setFlowAwareMarginEndForChild(child, 0);

        startEdge += flowAwareMarginStartForChild(child);

        LayoutUnit childLogicalWidth = flowAwareLogicalWidthForChild(child);
        bool shouldFlipInlineDirection = isColumnFlow() ? true : isLeftToRightFlow();
        LayoutUnit logicalLeft = shouldFlipInlineDirection ? startEdge : totalLogicalWidth - startEdge - childLogicalWidth;

        // FIXME: Supporting layout deltas.
        setFlowAwareLogicalLocationForChild(child, IntPoint(logicalLeft, logicalTop + flowAwareMarginBeforeForChild(child)));
        startEdge += childLogicalWidth + flowAwareMarginEndForChild(child);

        if (hasPackingSpace(availableFreeSpace, totalPositiveFlexibility) && style()->flexPack() == PackJustify && childSizes.size() > 1)
            startEdge += availableFreeSpace / (childSizes.size() - 1);
    }

    alignChildrenBlockDirection(iterator, maxAscent);
}

void RenderFlexibleBox::adjustLocationLogicalTopForChild(RenderBox* child, LayoutUnit delta)
{
    LayoutRect oldRect = child->frameRect();

    setFlowAwareLogicalLocationForChild(child, flowAwareLogicalLocationForChild(child) + LayoutSize(0, delta));

    // If the child moved, we have to repaint it as well as any floating/positioned
    // descendants. An exception is if we need a layout. In this case, we know we're going to
    // repaint ourselves (and the child) anyway.
    if (!selfNeedsLayout() && child->checkForRepaintDuringLayout())
        child->repaintDuringLayoutIfMoved(oldRect);
}

void RenderFlexibleBox::alignChildrenBlockDirection(FlexOrderIterator& iterator, LayoutUnit maxAscent)
{
    LayoutUnit logicalHeight = flowAwareLogicalHeight();

    for (RenderBox* child = iterator.first(); child; child = iterator.next()) {
        // direction:rtl + flex-flow:column means the cross-axis direction is flipped.
        if (!style()->isLeftToRightDirection() && isColumnFlow()) {
            LayoutPoint location = flowAwareLogicalLocationForChild(child);
            location.setY(logicalHeight - flowAwareLogicalHeightForChild(child) - location.y());
            setFlowAwareLogicalLocationForChild(child, location);
        }

        // FIXME: Make sure this does the right thing with column flows.
        switch (child->style()->flexAlign()) {
        case AlignStretch: {
            Length height = isHorizontalFlow() ? child->style()->height() : child->style()->width();
            if (height.isAuto()) {
                // FIXME: Clamp to max-height once it's spec'ed (should we align towards the start or center?).
                LayoutUnit stretchedHeight = logicalHeightForChild(child) + RenderFlexibleBox::availableLogicalHeightForChild(child);
                if (isHorizontalFlow())
                    child->setHeight(stretchedHeight);
                else
                    child->setWidth(stretchedHeight);
            }
            break;
        }
        case AlignStart:
            break;
        case AlignEnd:
            adjustLocationLogicalTopForChild(child, RenderFlexibleBox::availableLogicalHeightForChild(child));
            break;
        case AlignCenter:
            adjustLocationLogicalTopForChild(child, RenderFlexibleBox::availableLogicalHeightForChild(child) / 2);
            break;
        case AlignBaseline: {
            LayoutUnit ascent = marginBoxAscent(child);
            adjustLocationLogicalTopForChild(child, maxAscent - ascent);
            break;
        }
        }
    }
}

}

#endif // ENABLE(CSS3_FLEXBOX)
