/* -*- Mode: cc; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
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

#ifndef JS_SCRIPTABLE_H
#define JS_SCRIPTABLE_H

#include "jsIScriptable.h"
extern "C" {
#include <jsapi.h>
}

class jsScriptable: public jsIScriptable {
    JSObject *obj;
    JSString *className;
    jsIScriptable *proto, *parent;
 public:
    NS_DECL_ISUPPORTS;

    jsScriptable(jsIContext *, JSObject *);
    ~jsScriptable();
    JSString *getClassName();

    /* XXX use jsid for name/index? */
    jsval get(jsval id);
    JSBool has(jsval id);
    void put(jsval id, jsval v);
    void del(jsval id);

    jsIScriptable *getPrototype();
    void setPrototype(jsIScriptable *prototype);
    jsIScriptable *getParentScope();
    void setParentScope(jsIScriptable *parent);
    /* JSIdArray *getIds(); */
    jsval getDefaultValue(JSType hint);
    JSBool hasInstance(jsIScriptable *instance);
    JSObject *getJSObject(jsIContext *);
    void setJSObject(jsIContext *, JSObject *);
};

#endif /* JS_SCRIPTABLE_H */
