/*
 * RichEdit - painting functions
 *
 * Copyright 2004 by Krzysztof Foltman
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "editor.h"

WINE_DEFAULT_DEBUG_CHANNEL(richedit);

void ME_PaintContent(ME_TextEditor *editor, HDC hDC, BOOL bOnlyNew, RECT *rcUpdate) {
  ME_DisplayItem *item;
  ME_Context c;
  int yoffset;

  editor->nSequence++;
  yoffset = GetScrollPos(editor->hWnd, SB_VERT);
  ME_InitContext(&c, editor, hDC);
  SetBkMode(hDC, TRANSPARENT);
  ME_MoveCaret(editor);
  item = editor->pBuffer->pFirst->next;
  c.pt.y -= yoffset;
  while(item != editor->pBuffer->pLast) {
    assert(item->type == diParagraph);
    if (!bOnlyNew || (item->member.para.nFlags & MEPF_REPAINT))
    {
      BOOL bPaint = (rcUpdate == NULL);
      if (rcUpdate)
        bPaint = c.pt.y<rcUpdate->bottom && 
          c.pt.y+item->member.para.nHeight>rcUpdate->top;
      if (bPaint)
      {
        ME_DrawParagraph(&c, item);
        item->member.para.nFlags &= ~MEPF_REPAINT;
      }
    }
    c.pt.y += item->member.para.nHeight;
    item = item->member.para.next_para;
  }
  if (c.pt.y<c.rcView.bottom) {
    RECT rc;
    int xs = c.rcView.left, xe = c.rcView.right;
    int ys = c.pt.y, ye = c.rcView.bottom;
    
    if (bOnlyNew)
    {
      int y1 = editor->nTotalLength-yoffset, y2 = editor->nLastTotalLength-yoffset;
      if (y1<y2)
        ys = y1, ye = y2+1;
      else
        ys = ye;
    }
    
    if (rcUpdate && ys!=ye)
    {
      xs = rcUpdate->left, xe = rcUpdate->right;
      if (rcUpdate->top > ys)
        ys = rcUpdate->top;
      if (rcUpdate->bottom < ye)
        ye = rcUpdate->bottom;
    }
    
    rc.left = xs; /* FIXME remove if it's not necessary anymore */
    rc.top = c.pt.y;
    rc.right = xe;
    rc.bottom = c.pt.y+1;
    FillRect(hDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

    if (ys == c.pt.y) /* don't overwrite the top bar */
      ys++;
    if (ye>ys) {
      rc.left = xs;
      rc.top = ys;
      rc.right = xe;
      rc.bottom = ye;
      /* this is not supposed to be gray, I know, but lets keep it gray for now for debugging purposes */
      FillRect(hDC, &rc, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
    }
  }
  editor->nLastTotalLength = editor->nTotalLength;
  ME_DestroyContext(&c);
}

void ME_MarkParagraphRange(ME_TextEditor *editor, ME_DisplayItem *p1,
                           ME_DisplayItem *p2, int nFlags)
{
  ME_DisplayItem *p3;  
  if (p1 == p2)
  {
    p1->member.para.nFlags |= nFlags;
    return;
  }
  if (p1->member.para.nCharOfs > p2->member.para.nCharOfs)
    p3 = p1, p1 = p2, p2 = p3;
    
  p1->member.para.nFlags |= nFlags;
  do {
    p1 = p1->member.para.next_para;
    p1->member.para.nFlags |= nFlags;
  } while (p1 != p2);
}

void ME_MarkOffsetRange(ME_TextEditor *editor, int from, int to, int nFlags)
{
  ME_Cursor c1, c2;
  ME_CursorFromCharOfs(editor, from, &c1);
  ME_CursorFromCharOfs(editor, to, &c2);
  
  ME_MarkParagraphRange(editor, ME_GetParagraph(c1.pRun), ME_GetParagraph(c2.pRun), nFlags);
}

void ME_MarkSelectionForRepaint(ME_TextEditor *editor)
{
  int from, to, from2, to2, end;
  
  end = ME_GetTextLength(editor);
  ME_GetSelection(editor, &from, &to);
  from2 = editor->nLastSelStart;
  to2 = editor->nLastSelEnd;
  if (from<from2) ME_MarkOffsetRange(editor, from, from2, MEPF_REPAINT);
  if (from>from2) ME_MarkOffsetRange(editor, from2, from, MEPF_REPAINT);
  if (to<to2) ME_MarkOffsetRange(editor, to, to2, MEPF_REPAINT);
  if (to>to2) ME_MarkOffsetRange(editor, to2, to, MEPF_REPAINT);

  editor->nLastSelStart = from;
  editor->nLastSelEnd = to;
}

void ME_Repaint(ME_TextEditor *editor)
{
  ME_Cursor *pCursor = &editor->pCursors[0];
  ME_DisplayItem *pRun = NULL;
  int nOffset = -1;
  HDC hDC;
  int nCharOfs = ME_CharOfsFromRunOfs(editor, pCursor->pRun, pCursor->nOffset);
  
  ME_RunOfsFromCharOfs(editor, nCharOfs, &pRun, &nOffset);
  assert(pRun == pCursor->pRun);
  assert(nOffset == pCursor->nOffset);
  ME_MarkSelectionForRepaint(editor);
  ME_WrapMarkedParagraphs(editor);
  hDC = GetDC(editor->hWnd);
  ME_HideCaret(editor);
  ME_PaintContent(editor, hDC, TRUE, NULL);
  ReleaseDC(editor->hWnd, hDC);
  ME_ShowCaret(editor);
}

void ME_UpdateRepaint(ME_TextEditor *editor)
{
/*
  InvalidateRect(editor->hWnd, NULL, TRUE);
  */
  ME_SendOldNotify(editor, EN_CHANGE);
  ME_Repaint(editor);
  ME_SendOldNotify(editor, EN_UPDATE);
  ME_SendSelChange(editor);
}

void ME_DrawTextWithStyle(ME_Context *c, int x, int y, LPCWSTR szText, int nChars, 
  ME_Style *s, int *width, int nSelFrom, int nSelTo, int ymin, int cy) {
  HDC hDC = c->hDC;
  HGDIOBJ hOldFont;
  COLORREF rgbOld, rgbBack;
  hOldFont = ME_SelectStyleFont(c->editor, hDC, s);
  rgbBack = ME_GetBackColor(c->editor);
  if ((s->fmt.dwMask & CFM_COLOR) && (s->fmt.dwEffects & CFE_AUTOCOLOR))
    rgbOld = SetTextColor(hDC, GetSysColor(COLOR_WINDOWTEXT));
  else
    rgbOld = SetTextColor(hDC, s->fmt.crTextColor);
  ExtTextOutW(hDC, x, y, 0, NULL, szText, nChars, NULL);
  if (width) {
    SIZE sz;
    GetTextExtentPoint32W(hDC, szText, nChars, &sz);
    *width = sz.cx;
  }
  if (nSelFrom < nChars && nSelTo >= 0 && nSelFrom<nSelTo)
  {
    SIZE sz;
    if (nSelFrom < 0) nSelFrom = 0;
    if (nSelTo > nChars) nSelTo = nChars;
    GetTextExtentPoint32W(hDC, szText, nSelFrom, &sz);
    x += sz.cx;
    GetTextExtentPoint32W(hDC, szText+nSelFrom, nSelTo-nSelFrom, &sz);
    PatBlt(hDC, x, ymin, sz.cx, cy, DSTINVERT);
  }
  SetTextColor(hDC, rgbOld);
  ME_UnselectStyleFont(c->editor, hDC, s, hOldFont);
}

void ME_DebugWrite(HDC hDC, POINT *pt, WCHAR *szText) {
  int align = SetTextAlign(hDC, TA_LEFT|TA_TOP);
  HGDIOBJ hFont = SelectObject(hDC, GetStockObject(DEFAULT_GUI_FONT));
  COLORREF color = SetTextColor(hDC, RGB(128,128,128));
  TextOutW(hDC, pt->x, pt->y, szText, lstrlenW(szText));
  SelectObject(hDC, hFont);
  SetTextAlign(hDC, align);
  SetTextColor(hDC, color);
}

void ME_DrawGraphics(ME_Context *c, int x, int y, ME_Run *run, 
                     ME_Paragraph *para, BOOL selected) {
  SIZE sz;
  int xs, ys, xe, ye, h, ym, width, eyes;
  ME_GetGraphicsSize(c->editor, run, &sz);
  xs = run->pt.x;
  ys = y-sz.cy;
  xe = xs+sz.cx;
  ye = y;
  h = ye-ys;
  ym = ys+h/4;
  width = sz.cx;
  eyes = width/8;
  /* draw a smiling face :) */
  Ellipse(c->hDC, xs, ys, xe, ye);
  Ellipse(c->hDC, xs+width/8, ym, x+width/8+eyes, ym+eyes);
  Ellipse(c->hDC, xs+7*width/8-eyes, ym, xs+7*width/8, ym+eyes);
  MoveToEx(c->hDC, xs+width/8, ys+3*h/4-eyes, NULL);
  LineTo(c->hDC, xs+width/8, ys+3*h/4);
  LineTo(c->hDC, xs+7*width/8, ys+3*h/4);
  LineTo(c->hDC, xs+7*width/8, ys+3*h/4-eyes);
  if (selected)
  {
    /* descent is usually (always?) 0 for graphics */
    PatBlt(c->hDC, x, y-run->nAscent, sz.cx, run->nAscent+run->nDescent, DSTINVERT);    
  }
}

void ME_DrawRun(ME_Context *c, int x, int y, ME_DisplayItem *rundi, ME_Paragraph *para) {
  ME_Run *run = &rundi->member.run;
  int runofs = run->nCharOfs+para->nCharOfs;
  
  /* you can always comment it out if you need visible paragraph marks */
  if (run->nFlags & MERF_ENDPARA) 
    return;
  if (run->nFlags & MERF_GRAPHICS) {
    int blfrom, blto;
    ME_GetSelection(c->editor, &blfrom, &blto);
    ME_DrawGraphics(c, x, y, run, para, (runofs >= blfrom) && (runofs < blto));
  } else
  {
    int blfrom, blto;
    ME_DisplayItem *start = ME_FindItemBack(rundi, diStartRow);
    ME_GetSelection(c->editor, &blfrom, &blto);
    
    ME_DrawTextWithStyle(c, x, y, 
      run->strText->szData, ME_StrVLen(run->strText), run->style, NULL, 
        blfrom-runofs, blto-runofs, c->pt.y+start->member.row.nYPos, start->member.row.nHeight);
  }
}

COLORREF ME_GetBackColor(ME_TextEditor *editor)
{
/* Looks like I was seriously confused
    return GetSysColor((GetWindowLong(editor->hWnd, GWL_STYLE) & ES_READONLY) ? COLOR_3DFACE: COLOR_WINDOW);
*/
  if (editor->rgbBackColor == -1)
    return GetSysColor(COLOR_WINDOW);
  else
    return editor->rgbBackColor;
}

void ME_DrawParagraph(ME_Context *c, ME_DisplayItem *paragraph) {
  int align = SetTextAlign(c->hDC, TA_BASELINE);
  ME_DisplayItem *p;
  ME_Run *run;
  ME_Paragraph *para = NULL;
  RECT rc, rcPara;
  int y = c->pt.y;
  int height = 0, baseline = 0, no=0, pno = 0;
  int xs, xe;
  int visible = 0;
  int nMargWidth = 0;
  
  c->pt.x = c->rcView.left;
  rcPara.left = c->rcView.left;
  rcPara.right = c->rcView.right;
  for (p = paragraph; p!=paragraph->member.para.next_para; p = p->next) {
    switch(p->type) {
      case diParagraph:
        para = &p->member.para;
        break;
      case diStartRow:
        assert(para);
        nMargWidth = (pno==0?para->nFirstMargin:para->nLeftMargin);
        xs = c->rcView.left+nMargWidth;
        xe = c->rcView.right-para->nRightMargin;
        y += height;
        rcPara.top = y;
        rcPara.bottom = y+p->member.row.nHeight;
        visible = RectVisible(c->hDC, &rcPara);
        if (visible) {
          HBRUSH hbr;
          /* left margin */
          rc.left = c->rcView.left;
          rc.right = c->rcView.left+nMargWidth;
          rc.top = y;
          rc.bottom = y+p->member.row.nHeight;
          FillRect(c->hDC, &rc, c->hbrMargin);
          /* right margin */
          rc.left = xe;
          rc.right = c->rcView.right;
          FillRect(c->hDC, &rc, c->hbrMargin);
          rc.left = c->rcView.left+para->nLeftMargin;
          rc.right = xe;
          hbr = CreateSolidBrush(ME_GetBackColor(c->editor));
          FillRect(c->hDC, &rc, hbr);
          DeleteObject(hbr);
        }
        if (me_debug)
        {
          const WCHAR wszRowDebug[] = {'r','o','w','[','%','d',']',0};
          WCHAR buf[128];
          POINT pt = c->pt;
          wsprintfW(buf, wszRowDebug, no);
          pt.y = 12+y;
          ME_DebugWrite(c->hDC, &pt, buf);
        }
        
        height = p->member.row.nHeight;
        baseline = p->member.row.nBaseline;
        pno++;
        break;
      case diRun:
        assert(para);
        run = &p->member.run;
        if (visible && me_debug) {
          rc.left = c->rcView.left+run->pt.x;
          rc.right = c->rcView.left+run->pt.x+run->nWidth;
          rc.top = c->pt.y+run->pt.y;
          rc.bottom = c->pt.y+run->pt.y+height;
          TRACE("rc = (%ld, %ld, %ld, %ld)\n", rc.left, rc.top, rc.right, rc.bottom);
          if (run->nFlags & MERF_SKIPPED)
            DrawFocusRect(c->hDC, &rc);
          else
            FrameRect(c->hDC, &rc, GetSysColorBrush(COLOR_GRAYTEXT));
        }
        if (visible)
          ME_DrawRun(c, run->pt.x, c->pt.y+run->pt.y+baseline, p, &paragraph->member.para);
        if (me_debug)
        {
          /* I'm using %ls, hope wsprintfW is not going to use wrong (4-byte) WCHAR version */
          const WCHAR wszRunDebug[] = {'[','%','d',':','%','x',']',' ','%','l','s',0};
          WCHAR buf[2560];
          POINT pt;
          pt.x = run->pt.x;
          pt.y = c->pt.y + run->pt.y;
          wsprintfW(buf, wszRunDebug, no, p->member.run.nFlags, p->member.run.strText->szData);
          ME_DebugWrite(c->hDC, &pt, buf);
        }
        /* c->pt.x += p->member.run.nWidth; */
        break;
      default:
        break;
    }
    no++;
  }
  SetTextAlign(c->hDC, align);
}

void ME_UpdateScrollBar(ME_TextEditor *editor, int ypos)
{
  float perc = 0.0;
  SCROLLINFO si;
  HWND hWnd = editor->hWnd;
  int overflow = editor->nTotalLength - editor->sizeWindow.cy;
  si.cbSize = sizeof(SCROLLINFO);
  si.fMask = SIF_PAGE|SIF_POS|SIF_RANGE|SIF_TRACKPOS;
  GetScrollInfo(hWnd, SB_VERT, &si);
  
  if (ypos < 0) {
    if (si.nMax<1) si.nMax = 1;
    perc = 1.0*si.nPos/si.nMax;
    ypos = perc*overflow;
  }
  if (ypos >= overflow && overflow > 0)
    ypos = overflow - 1;

  if (overflow > 0) {
    EnableScrollBar(hWnd, SB_VERT, ESB_ENABLE_BOTH);
    SetScrollRange(hWnd, SB_VERT, 0, overflow, FALSE);
    SetScrollPos(hWnd, SB_VERT, ypos, TRUE);
  } else {
    EnableScrollBar(hWnd, SB_VERT, ESB_DISABLE_BOTH);
    SetScrollRange(hWnd, SB_VERT, 0, 0, FALSE);
    SetScrollPos(hWnd, SB_VERT, 0, TRUE);
  }
  if (ypos != si.nPos)
  {
    TRACE("ScrollWindow(%d, %d, %d, %0.4f)\n", si.nPos, si.nMax, ypos, perc);
    ScrollWindow(hWnd, 0, si.nPos - ypos, NULL, NULL);
    UpdateWindow(hWnd);
  }
}

int ME_GetScrollPos(ME_TextEditor *editor)
{
  return GetScrollPos(editor->hWnd, SB_VERT);
}

void ME_EnsureVisible(ME_TextEditor *editor, ME_DisplayItem *pRun)
{
  ME_DisplayItem *pRow = ME_FindItemBack(pRun, diStartRow);
  ME_DisplayItem *pPara = ME_FindItemBack(pRun, diParagraph);
  int y, yrel, yheight;
  
  assert(pRow);
  assert(pPara);
  
  y = pPara->member.para.nYPos+pRow->member.row.nYPos;
  yheight = pRow->member.row.nHeight;
  yrel = y - ME_GetScrollPos(editor);
  if (yrel < 0)
    ME_UpdateScrollBar(editor, y);
  else if (yrel + yheight > editor->sizeWindow.cy)
  {
    ME_UpdateScrollBar(editor, y + yheight - editor->sizeWindow.cy);  
  }
}
