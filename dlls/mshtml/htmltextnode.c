/*
 * Copyright 2008 Jacek Caban for CodeWeavers
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */


#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "ole2.h"

#include "mshtml_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mshtml);

struct HTMLDOMTextNode {
    HTMLDOMNode node;
    IHTMLDOMTextNode IHTMLDOMTextNode_iface;
    IHTMLDOMTextNode2 IHTMLDOMTextNode2_iface;

    nsIDOMText *nstext;
};

static inline HTMLDOMTextNode *impl_from_IHTMLDOMTextNode(IHTMLDOMTextNode *iface)
{
    return CONTAINING_RECORD(iface, HTMLDOMTextNode, IHTMLDOMTextNode_iface);
}

DISPEX_IDISPATCH_IMPL(HTMLDOMTextNode, IHTMLDOMTextNode,
                      impl_from_IHTMLDOMTextNode(iface)->node.event_target.dispex)

static HRESULT WINAPI HTMLDOMTextNode_put_data(IHTMLDOMTextNode *iface, BSTR v)
{
    HTMLDOMTextNode *This = impl_from_IHTMLDOMTextNode(iface);
    nsAString nsstr;
    nsresult nsres;

    TRACE("(%p)->(%s)\n", This, debugstr_w(v));

    nsAString_InitDepend(&nsstr, v);
    nsres = nsIDOMText_SetData(This->nstext, &nsstr);
    nsAString_Finish(&nsstr);
    return NS_SUCCEEDED(nsres) ? S_OK : E_FAIL;
}

static HRESULT WINAPI HTMLDOMTextNode_get_data(IHTMLDOMTextNode *iface, BSTR *p)
{
    HTMLDOMTextNode *This = impl_from_IHTMLDOMTextNode(iface);
    nsAString nsstr;
    nsresult nsres;

    TRACE("(%p)->(%p)\n", This, p);

    nsAString_Init(&nsstr, NULL);
    nsres = nsIDOMText_GetData(This->nstext, &nsstr);
    return return_nsstr(nsres, &nsstr, p);
}

static HRESULT WINAPI HTMLDOMTextNode_toString(IHTMLDOMTextNode *iface, BSTR *String)
{
    HTMLDOMTextNode *This = impl_from_IHTMLDOMTextNode(iface);

    TRACE("(%p)->(%p)\n", This, String);

    if(!String)
        return E_INVALIDARG;

    if(dispex_compat_mode(&This->node.event_target.dispex) < COMPAT_MODE_IE9)
        return IHTMLDOMTextNode_get_data(&This->IHTMLDOMTextNode_iface, String);

    return dispex_to_string(&This->node.event_target.dispex, String);
}

static HRESULT WINAPI HTMLDOMTextNode_get_length(IHTMLDOMTextNode *iface, LONG *p)
{
    HTMLDOMTextNode *This = impl_from_IHTMLDOMTextNode(iface);
    UINT32 length = 0;
    nsresult nsres;

    TRACE("(%p)->(%p)\n", This, p);

    nsres = nsIDOMText_GetLength(This->nstext, &length);
    if(NS_FAILED(nsres))
        ERR("GetLength failed: %08lx\n", nsres);

    *p = length;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMTextNode_splitText(IHTMLDOMTextNode *iface, LONG offset, IHTMLDOMNode **pRetNode)
{
    HTMLDOMTextNode *This = impl_from_IHTMLDOMTextNode(iface);
    HTMLDOMNode *node;
    nsIDOMText *text;
    nsresult nsres;
    HRESULT hres;

    TRACE("(%p)->(%ld %p)\n", This, offset, pRetNode);

    nsres = nsIDOMText_SplitText(This->nstext, offset, &text);
    if(NS_FAILED(nsres)) {
        ERR("SplitText failed: %lx08x\n", nsres);
        return E_FAIL;
    }

    if(!text) {
        *pRetNode = NULL;
        return S_OK;
    }

    hres = get_node((nsIDOMNode*)text, TRUE, &node);
    nsIDOMText_Release(text);
    if(FAILED(hres))
        return hres;

    *pRetNode = &node->IHTMLDOMNode_iface;
    return S_OK;
}

static const IHTMLDOMTextNodeVtbl HTMLDOMTextNodeVtbl = {
    HTMLDOMTextNode_QueryInterface,
    HTMLDOMTextNode_AddRef,
    HTMLDOMTextNode_Release,
    HTMLDOMTextNode_GetTypeInfoCount,
    HTMLDOMTextNode_GetTypeInfo,
    HTMLDOMTextNode_GetIDsOfNames,
    HTMLDOMTextNode_Invoke,
    HTMLDOMTextNode_put_data,
    HTMLDOMTextNode_get_data,
    HTMLDOMTextNode_toString,
    HTMLDOMTextNode_get_length,
    HTMLDOMTextNode_splitText
};

static inline HTMLDOMTextNode *impl_from_IHTMLDOMTextNode2(IHTMLDOMTextNode2 *iface)
{
    return CONTAINING_RECORD(iface, HTMLDOMTextNode, IHTMLDOMTextNode2_iface);
}

DISPEX_IDISPATCH_IMPL(HTMLDOMTextNode2, IHTMLDOMTextNode2,
                      impl_from_IHTMLDOMTextNode2(iface)->node.event_target.dispex)

static HRESULT WINAPI HTMLDOMTextNode2_substringData(IHTMLDOMTextNode2 *iface, LONG offset, LONG count, BSTR *string)
{
    HTMLDOMTextNode *This = impl_from_IHTMLDOMTextNode2(iface);
    FIXME("(%p)->(%ld %ld %p)\n", This, offset, count, string);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMTextNode2_appendData(IHTMLDOMTextNode2 *iface, BSTR string)
{
    HTMLDOMTextNode *This = impl_from_IHTMLDOMTextNode2(iface);
    nsAString nsstr;
    nsresult nsres;

    TRACE("(%p)->(%s)\n", This, debugstr_w(string));

    nsAString_InitDepend(&nsstr, string);
    nsres = nsIDOMText_AppendData(This->nstext, &nsstr);
    nsAString_Finish(&nsstr);
    if(NS_FAILED(nsres)) {
        ERR("AppendData failed: %08lx\n", nsres);
        return E_FAIL;
    }

    return S_OK;
}

static HRESULT WINAPI HTMLDOMTextNode2_insertData(IHTMLDOMTextNode2 *iface, LONG offset, BSTR string)
{
    HTMLDOMTextNode *This = impl_from_IHTMLDOMTextNode2(iface);
    FIXME("(%p)->(%ld %s)\n", This, offset, debugstr_w(string));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMTextNode2_deleteData(IHTMLDOMTextNode2 *iface, LONG offset, LONG count)
{
    HTMLDOMTextNode *This = impl_from_IHTMLDOMTextNode2(iface);
    FIXME("(%p)->(%ld %ld)\n", This, offset, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMTextNode2_replaceData(IHTMLDOMTextNode2 *iface, LONG offset, LONG count, BSTR string)
{
    HTMLDOMTextNode *This = impl_from_IHTMLDOMTextNode2(iface);
    FIXME("(%p)->(%ld %ld %s)\n", This, offset, count, debugstr_w(string));
    return E_NOTIMPL;
}

static const IHTMLDOMTextNode2Vtbl HTMLDOMTextNode2Vtbl = {
    HTMLDOMTextNode2_QueryInterface,
    HTMLDOMTextNode2_AddRef,
    HTMLDOMTextNode2_Release,
    HTMLDOMTextNode2_GetTypeInfoCount,
    HTMLDOMTextNode2_GetTypeInfo,
    HTMLDOMTextNode2_GetIDsOfNames,
    HTMLDOMTextNode2_Invoke,
    HTMLDOMTextNode2_substringData,
    HTMLDOMTextNode2_appendData,
    HTMLDOMTextNode2_insertData,
    HTMLDOMTextNode2_deleteData,
    HTMLDOMTextNode2_replaceData
};

static inline HTMLDOMTextNode *impl_from_HTMLDOMNode(HTMLDOMNode *iface)
{
    return CONTAINING_RECORD(iface, HTMLDOMTextNode, node);
}

static HRESULT HTMLDOMTextNode_clone(HTMLDOMNode *iface, nsIDOMNode *nsnode, HTMLDOMNode **ret)
{
    HTMLDOMTextNode *This = impl_from_HTMLDOMNode(iface);

    return HTMLDOMTextNode_Create(This->node.doc, nsnode, ret);
}

static inline HTMLDOMTextNode *impl_from_DispatchEx(DispatchEx *iface)
{
    return CONTAINING_RECORD(iface, HTMLDOMTextNode, node.event_target.dispex);
}

static void *HTMLDOMTextNode_query_interface(DispatchEx *dispex, REFIID riid)
{
    HTMLDOMTextNode *This = impl_from_DispatchEx(dispex);

    if(IsEqualGUID(&IID_IHTMLDOMTextNode, riid))
        return &This->IHTMLDOMTextNode_iface;
    if(IsEqualGUID(&IID_IHTMLDOMTextNode2, riid))
        return &This->IHTMLDOMTextNode2_iface;

    return HTMLDOMNode_query_interface(&This->node.event_target.dispex, riid);
}

static const cpc_entry_t HTMLDOMTextNode_cpc[] = {{NULL}};

static const NodeImplVtbl HTMLDOMTextNodeImplVtbl = {
    .cpc_entries           = HTMLDOMTextNode_cpc,
    .clone                 = HTMLDOMTextNode_clone
};

dispex_static_data_t CharacterData_dispex = {
    .id           = PROT_CharacterData,
    .prototype_id = PROT_Node,
};

static const dispex_static_data_vtbl_t Text_dispex_vtbl = {
    .query_interface = HTMLDOMTextNode_query_interface,
    .destructor      = HTMLDOMNode_destructor,
    .traverse        = HTMLDOMNode_traverse,
    .unlink          = HTMLDOMNode_unlink
};

static const tid_t Text_iface_tids[] = {
    IHTMLDOMNode_tid,
    IHTMLDOMNode2_tid,
    IHTMLDOMTextNode_tid,
    IHTMLDOMTextNode2_tid,
    0
};
dispex_static_data_t Text_dispex = {
    .id           = PROT_Text,
    .prototype_id = PROT_CharacterData,
    .vtbl         = &Text_dispex_vtbl,
    .disp_tid     = DispHTMLDOMTextNode_tid,
    .iface_tids   = Text_iface_tids,
    .init_info    = HTMLDOMNode_init_dispex_info,
};

HRESULT HTMLDOMTextNode_Create(HTMLDocumentNode *doc, nsIDOMNode *nsnode, HTMLDOMNode **node)
{
    HTMLDOMTextNode *ret;
    nsresult nsres;

    ret = calloc(1, sizeof(*ret));
    if(!ret)
        return E_OUTOFMEMORY;

    ret->node.vtbl = &HTMLDOMTextNodeImplVtbl;
    ret->IHTMLDOMTextNode_iface.lpVtbl = &HTMLDOMTextNodeVtbl;
    ret->IHTMLDOMTextNode2_iface.lpVtbl = &HTMLDOMTextNode2Vtbl;

    HTMLDOMNode_Init(doc, &ret->node, nsnode, &Text_dispex);

    nsres = nsIDOMNode_QueryInterface(nsnode, &IID_nsIDOMText, (void**)&ret->nstext);
    assert(nsres == NS_OK && (nsIDOMNode*)ret->nstext == ret->node.nsnode);

    /* Share reference with nsnode */
    nsIDOMNode_Release(ret->node.nsnode);

    *node = &ret->node;
    return S_OK;
}
