/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */
#include "nsHTMLParts.h"
#include "nsHTMLContainer.h"
#include "nsBodyFrame.h"
#include "nsIPresContext.h"
#include "nsHTMLIIDs.h"
#include "nsIWebShell.h"
#include "nsHTMLAtoms.h"
#include "nsIStyleRule.h"
#include "nsIStyleContext.h"

static NS_DEFINE_IID(kIWebShellIID, NS_IWEB_SHELL_IID);
static NS_DEFINE_IID(kIStyleRuleIID, NS_ISTYLE_RULE_IID);

class BodyPart;

class BodyRule: public nsIStyleRule {
public:
  BodyRule(BodyPart* aPart);
  ~BodyRule();

  NS_DECL_ISUPPORTS

  virtual PRBool Equals(const nsIStyleRule* aRule) const;
  virtual PRUint32 HashValue(void) const;

  virtual void MapStyleInto(nsIStyleContext* aContext, nsIPresContext* aPresContext);

  virtual void List(FILE* out = stdout, PRInt32 aIndent = 0) const;

  BodyPart* mPart;
};

class BodyPart : public nsHTMLContainer {
public:
  BodyPart(nsIAtom* aTag);

  NS_IMETHOD_(nsrefcnt) AddRef();
  NS_IMETHOD_(nsrefcnt) Release();

  virtual nsresult CreateFrame(nsIPresContext* aPresContext,
                               nsIFrame* aParentFrame,
                               nsIStyleContext* aStyleContext,
                               nsIFrame*& aResult);

  virtual nsIStyleRule* GetStyleRule(void);

protected:
  virtual ~BodyPart();

  BodyRule* mStyleRule;
};


// -----------------------------------------------------------


BodyRule::BodyRule(BodyPart* aPart)
{
  NS_INIT_REFCNT();
  mPart = aPart;
}

BodyRule::~BodyRule()
{
}

NS_IMPL_ISUPPORTS(BodyRule, kIStyleRuleIID);

PRBool BodyRule::Equals(const nsIStyleRule* aRule) const
{
  return PRBool(this == aRule);
}

PRUint32 BodyRule::HashValue(void) const
{
  return (PRUint32)(mPart);
}

void BodyRule::MapStyleInto(nsIStyleContext* aContext, nsIPresContext* aPresContext)
{
  if (nsnull != mPart) {

    nsStyleSpacing* styleSpacing = (nsStyleSpacing*)(aContext->GetMutableStyleData(eStyleStruct_Spacing));

    if (nsnull != styleSpacing) {
      nsHTMLValue   value;
      nsStyleCoord  zero(0);
      PRInt32       count = 0;
      PRInt32       attrCount = mPart->GetAttributeCount();

      if (0 < attrCount) {
        // if marginwidth/marginheigth is set in our attribute zero out left,right/top,bottom padding
        // nsBodyFrame::DidSetStyleContext will add the appropriate values to padding 
        mPart->GetAttribute(nsHTMLAtoms::marginwidth, value);
        if (eHTMLUnit_Pixel == value.GetUnit()) {
          styleSpacing->mPadding.SetLeft(zero);
          styleSpacing->mPadding.SetRight(zero);
          count++;
        }

        mPart->GetAttribute(nsHTMLAtoms::marginheight, value);
        if (eHTMLUnit_Pixel == value.GetUnit()) {
          styleSpacing->mPadding.SetTop(zero);
          styleSpacing->mPadding.SetBottom(zero);
          count++;
        }

        if (count < attrCount) {  // more to go...
          mPart->MapAttributesInto(aContext, aPresContext);
        }
      }

      if (count < 2) {
        // if marginwidth or marginheight is set in the web shell zero out left,right,top,bottom padding
        // nsBodyFrame::DidSetStyleContext will add the appropriate values to padding 
        nsISupports* container;
        aPresContext->GetContainer(&container);
        if (nsnull != container) {
          nsIWebShell* webShell = nsnull;
          container->QueryInterface(kIWebShellIID, (void**) &webShell);
          if (nsnull != webShell) {
            PRInt32 marginWidth, marginHeight;
            webShell->GetMarginWidth(marginWidth);
            webShell->GetMarginHeight(marginHeight);
            if ((marginWidth >= 0) || (marginHeight >= 0)) { // nav quirk
              styleSpacing->mPadding.SetLeft(zero);
              styleSpacing->mPadding.SetRight(zero);
              styleSpacing->mPadding.SetTop(zero);
              styleSpacing->mPadding.SetBottom(zero);
            }
            NS_RELEASE(webShell);
          }
          NS_RELEASE(container);
        }
      }
    }
  }
}

void BodyRule::List(FILE* out, PRInt32 aIndent) const
{
}

// -----------------------------------------------------------

BodyPart::BodyPart(nsIAtom* aTag)
  : nsHTMLContainer(aTag),
    mStyleRule(nsnull)
{
}

BodyPart::~BodyPart()
{
  if (nsnull != mStyleRule) {
    mStyleRule->mPart = nsnull;
    NS_RELEASE(mStyleRule);
  }
}

nsrefcnt BodyPart::AddRef(void)
{
  return ++mRefCnt;
}

nsrefcnt BodyPart::Release(void)
{
  if (--mRefCnt == 0) {
    delete this;
    return 0;
  }
  return mRefCnt;
}

nsresult
BodyPart::CreateFrame(nsIPresContext*  aPresContext,
                      nsIFrame*        aParentFrame,
                      nsIStyleContext* aStyleContext,
                      nsIFrame*&       aResult)
{
  nsIFrame* frame = nsnull;
  nsresult rv = nsBodyFrame::NewFrame(&frame, this, aParentFrame);
  if (NS_OK != rv) {
    return rv;
  }
  frame->SetStyleContext(aPresContext, aStyleContext);
  aResult = frame;
  return NS_OK;
}

nsIStyleRule* BodyPart::GetStyleRule(void)
{
  if (nsnull == mStyleRule) {
    mStyleRule = new BodyRule(this);
    NS_IF_ADDREF(mStyleRule);
  }
  NS_IF_ADDREF(mStyleRule);
  return mStyleRule;
}


nsresult
NS_NewBodyPart(nsIHTMLContent** aInstancePtrResult,
               nsIAtom* aTag)
{
  NS_PRECONDITION(nsnull != aInstancePtrResult, "null ptr");
  if (nsnull == aInstancePtrResult) {
    return NS_ERROR_NULL_POINTER;
  }
  nsIHTMLContent* body = new BodyPart(aTag);
  if (nsnull == body) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  return body->QueryInterface(kIHTMLContentIID, (void **) aInstancePtrResult);
}
