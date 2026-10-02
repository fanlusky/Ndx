#include "tsf/tsf.h"
#include "nvim/nvim.h"
#include "renderer/renderer.h"

#include <olectl.h>

constexpr TsViewCookie TSF_VIEW_COOKIE = 1;
constexpr ULONG MAX_REQUESTED_ATTRS = 16;
constexpr ULONG MAX_DISPLAY_ATTRIBUTE_PROPERTIES = 16;

// The text store only ever holds the text which is currently being composed by the
// IME. As soon as text is no longer part of a composition, it is sent to nvim
// and removed from the store.
class TsfTextStore : public ITextStoreACP, public ITfContextOwnerCompositionSink, public ITfTextEditSink {
public:
	TsfTextStore(HWND hwnd, Renderer *renderer, Nvim *nvim);
	~TsfTextStore();

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv_object) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;

	// ITextStoreACP
	STDMETHODIMP AdviseSink(REFIID riid, IUnknown *punk, DWORD dwMask) override;
	STDMETHODIMP UnadviseSink(IUnknown *punk) override;
	STDMETHODIMP RequestLock(DWORD dwLockFlags, HRESULT *phrSession) override;
	STDMETHODIMP GetStatus(TS_STATUS *pdcs) override;
	STDMETHODIMP QueryInsert(LONG acpTestStart, LONG acpTestEnd, ULONG cch, LONG *pacpResultStart,
		LONG *pacpResultEnd) override;
	STDMETHODIMP GetSelection(ULONG ulIndex, ULONG ulCount, TS_SELECTION_ACP *pSelection, ULONG *pcFetched) override;
	STDMETHODIMP SetSelection(ULONG ulCount, const TS_SELECTION_ACP *pSelection) override;
	STDMETHODIMP GetText(LONG acpStart, LONG acpEnd, WCHAR *pchPlain, ULONG cchPlainReq, ULONG *pcchPlainRet,
		TS_RUNINFO *prgRunInfo, ULONG cRunInfoReq, ULONG *pcRunInfoRet, LONG *pacpNext) override;
	STDMETHODIMP SetText(DWORD dwFlags, LONG acpStart, LONG acpEnd, const WCHAR *pchText, ULONG cch,
		TS_TEXTCHANGE *pChange) override;
	STDMETHODIMP GetFormattedText(LONG acpStart, LONG acpEnd, IDataObject **ppDataObject) override;
	STDMETHODIMP GetEmbedded(LONG acpPos, REFGUID rguidService, REFIID riid, IUnknown **ppunk) override;
	STDMETHODIMP QueryInsertEmbedded(const GUID *pguidService, const FORMATETC *pFormatEtc, BOOL *pfInsertable) override;
	STDMETHODIMP InsertEmbedded(DWORD dwFlags, LONG acpStart, LONG acpEnd, IDataObject *pDataObject,
		TS_TEXTCHANGE *pChange) override;
	STDMETHODIMP InsertTextAtSelection(DWORD dwFlags, const WCHAR *pchText, ULONG cch, LONG *pacpStart,
		LONG *pacpEnd, TS_TEXTCHANGE *pChange) override;
	STDMETHODIMP InsertEmbeddedAtSelection(DWORD dwFlags, IDataObject *pDataObject, LONG *pacpStart, LONG *pacpEnd,
		TS_TEXTCHANGE *pChange) override;
	STDMETHODIMP RequestSupportedAttrs(DWORD dwFlags, ULONG cFilterAttrs, const TS_ATTRID *paFilterAttrs) override;
	STDMETHODIMP RequestAttrsAtPosition(LONG acpPos, ULONG cFilterAttrs, const TS_ATTRID *paFilterAttrs,
		DWORD dwFlags) override;
	STDMETHODIMP RequestAttrsTransitioningAtPosition(LONG acpPos, ULONG cFilterAttrs, const TS_ATTRID *paFilterAttrs,
		DWORD dwFlags) override;
	STDMETHODIMP FindNextAttrTransition(LONG acpStart, LONG acpHalt, ULONG cFilterAttrs, const TS_ATTRID *paFilterAttrs,
		DWORD dwFlags, LONG *pacpNext, BOOL *pfFound, LONG *plFoundOffset) override;
	STDMETHODIMP RetrieveRequestedAttrs(ULONG ulCount, TS_ATTRVAL *paAttrVals, ULONG *pcFetched) override;
	STDMETHODIMP GetEndACP(LONG *pacp) override;
	STDMETHODIMP GetActiveView(TsViewCookie *pvcView) override;
	STDMETHODIMP GetACPFromPoint(TsViewCookie vcView, const POINT *ptScreen, DWORD dwFlags, LONG *pacp) override;
	STDMETHODIMP GetTextExt(TsViewCookie vcView, LONG acpStart, LONG acpEnd, RECT *prc, BOOL *pfClipped) override;
	STDMETHODIMP GetScreenExt(TsViewCookie vcView, RECT *prc) override;
	STDMETHODIMP GetWnd(TsViewCookie vcView, HWND *phwnd) override;

	// ITfContextOwnerCompositionSink
	STDMETHODIMP OnStartComposition(ITfCompositionView *pComposition, BOOL *pfOk) override;
	STDMETHODIMP OnUpdateComposition(ITfCompositionView *pComposition, ITfRange *pRangeNew) override;
	STDMETHODIMP OnEndComposition(ITfCompositionView *pComposition) override;

	// ITfTextEditSink
	STDMETHODIMP OnEndEdit(ITfContext *pic, TfEditCookie ecReadOnly, ITfEditRecord *pEditRecord) override;

	void Attach(ITfContext *context);
	void Detach();
	void NotifyLayoutChange();
	void TerminateComposition();
	bool IsComposing() { return composition_view != nullptr; }
	void ProcessPending();

private:
	bool HasReadLock() { return (lock_flags & TS_LF_READ) == TS_LF_READ; }
	bool HasReadWriteLock() { return (lock_flags & TS_LF_READWRITE) == TS_LF_READWRITE; }
	bool ReplaceText(LONG start, LONG end, const wchar_t *new_text, ULONG new_length);
	void RemoveTextPrefix(LONG count);
	LONG GetCompositionStart();
	void SetCompositionView(ITfCompositionView *view);
	void CollectDisplayAttributeProperties();
	bool GetDisplayAttribute(TfGuidAtom guid_atom, TF_DISPLAYATTRIBUTE *attribute);
	void UpdateClauses(ITfContext *context, TfEditCookie ec);
	void StoreRequestedAttrs(ULONG count, const TS_ATTRID *attrs);

	ULONG ref_count;
	HWND hwnd;
	Renderer *renderer;
	Nvim *nvim;

	ITextStoreACPSink *sink;
	DWORD sink_mask;
	DWORD lock_flags;
	DWORD pending_lock_flags;
	bool layout_change_pending;
	bool composition_dirty;
	bool processing;
	bool locked_while_processing;

	// Weak reference, the context owns the text store
	ITfContext *context;
	DWORD edit_sink_cookie;
	ITfCategoryMgr *category_mgr;
	ITfDisplayAttributeMgr *display_attribute_mgr;
	GUID display_attribute_properties[MAX_DISPLAY_ATTRIBUTE_PROPERTIES];
	ULONG display_attribute_property_count;
	ITfCompositionView *composition_view;

	wchar_t *text;
	LONG text_length;
	LONG text_capacity;
	LONG selection_start;
	LONG selection_end;

	CompositionClause clauses[MAX_COMPOSITION_CLAUSES];
	uint32_t clause_count;

	TS_ATTRID requested_attrs[MAX_REQUESTED_ATTRS];
	ULONG requested_attr_count;
};

TsfTextStore::TsfTextStore(HWND hwnd, Renderer *renderer, Nvim *nvim) :
	ref_count(1), hwnd(hwnd), renderer(renderer), nvim(nvim),
	sink(nullptr), sink_mask(0), lock_flags(0), pending_lock_flags(0),
	layout_change_pending(false), composition_dirty(false), processing(false), locked_while_processing(false),
	context(nullptr), edit_sink_cookie(TF_INVALID_COOKIE), category_mgr(nullptr),
	display_attribute_mgr(nullptr), display_attribute_property_count(0), composition_view(nullptr),
	text(nullptr), text_length(0), text_capacity(0), selection_start(0), selection_end(0),
	clause_count(0), requested_attr_count(0) {
	CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
		reinterpret_cast<void **>(&category_mgr));
	CoCreateInstance(CLSID_TF_DisplayAttributeMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfDisplayAttributeMgr,
		reinterpret_cast<void **>(&display_attribute_mgr));
	CollectDisplayAttributeProperties();
}

TsfTextStore::~TsfTextStore() {
	Detach();
	SafeRelease(&sink);
	SafeRelease(&composition_view);
	SafeRelease(&category_mgr);
	SafeRelease(&display_attribute_mgr);
	free(text);
}

STDMETHODIMP TsfTextStore::QueryInterface(REFIID riid, void **ppv_object) {
	if (!ppv_object) return E_INVALIDARG;

	if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITextStoreACP)) {
		*ppv_object = static_cast<ITextStoreACP *>(this);
	}
	else if (IsEqualIID(riid, IID_ITfContextOwnerCompositionSink)) {
		*ppv_object = static_cast<ITfContextOwnerCompositionSink *>(this);
	}
	else if (IsEqualIID(riid, IID_ITfTextEditSink)) {
		*ppv_object = static_cast<ITfTextEditSink *>(this);
	}
	else {
		*ppv_object = nullptr;
		return E_NOINTERFACE;
	}

	AddRef();
	return S_OK;
}

STDMETHODIMP_(ULONG) TsfTextStore::AddRef() {
	return ++ref_count;
}

STDMETHODIMP_(ULONG) TsfTextStore::Release() {
	ULONG new_count = --ref_count;
	if (new_count == 0) {
		delete this;
	}
	return new_count;
}

void TsfTextStore::Attach(ITfContext *new_context) {
	context = new_context;

	ITfSource *source;
	if (SUCCEEDED(context->QueryInterface(IID_ITfSource, reinterpret_cast<void **>(&source)))) {
		if (FAILED(source->AdviseSink(IID_ITfTextEditSink, static_cast<ITfTextEditSink *>(this), &edit_sink_cookie))) {
			edit_sink_cookie = TF_INVALID_COOKIE;
		}
		source->Release();
	}
}

void TsfTextStore::Detach() {
	if (context && edit_sink_cookie != TF_INVALID_COOKIE) {
		ITfSource *source;
		if (SUCCEEDED(context->QueryInterface(IID_ITfSource, reinterpret_cast<void **>(&source)))) {
			source->UnadviseSink(edit_sink_cookie);
			source->Release();
		}
	}
	edit_sink_cookie = TF_INVALID_COOKIE;
	context = nullptr;
}

//
// ITextStoreACP
//

STDMETHODIMP TsfTextStore::AdviseSink(REFIID riid, IUnknown *punk, DWORD dwMask) {
	if (!IsEqualGUID(riid, IID_ITextStoreACPSink)) return E_INVALIDARG;
	if (!punk) return E_INVALIDARG;

	if (sink) {
		// Same sink, only update the mask
		IUnknown *new_unknown = nullptr;
		IUnknown *old_unknown = nullptr;
		punk->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&new_unknown));
		sink->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&old_unknown));
		bool same_sink = new_unknown && new_unknown == old_unknown;
		SafeRelease(&new_unknown);
		SafeRelease(&old_unknown);
		if (!same_sink) return CONNECT_E_ADVISELIMIT;

		sink_mask = dwMask;
		return S_OK;
	}

	if (FAILED(punk->QueryInterface(IID_ITextStoreACPSink, reinterpret_cast<void **>(&sink)))) {
		sink = nullptr;
		return E_NOINTERFACE;
	}
	sink_mask = dwMask;
	return S_OK;
}

STDMETHODIMP TsfTextStore::UnadviseSink(IUnknown *punk) {
	SafeRelease(&sink);
	sink_mask = 0;
	return S_OK;
}

STDMETHODIMP TsfTextStore::RequestLock(DWORD dwLockFlags, HRESULT *phrSession) {
	if (!sink) return E_UNEXPECTED;
	if (!phrSession) return E_INVALIDARG;

	DWORD requested_lock = dwLockFlags & TS_LF_READWRITE;
	if (lock_flags) {
		// Already locked, synchronous requests have to be denied,
		// asynchronous ones are granted once the current lock is released
		if (dwLockFlags & TS_LF_SYNC) {
			*phrSession = TS_E_SYNCHRONOUS;
			return S_OK;
		}
		pending_lock_flags |= requested_lock;
		*phrSession = TS_S_ASYNC;
		return S_OK;
	}

	AddRef();
	if (processing) {
		locked_while_processing = true;
	}
	lock_flags = requested_lock;
	*phrSession = sink->OnLockGranted(requested_lock);
	lock_flags = 0;

	while (pending_lock_flags && sink) {
		lock_flags = pending_lock_flags;
		pending_lock_flags = 0;
		sink->OnLockGranted(lock_flags);
		lock_flags = 0;
	}

	ProcessPending();
	Release();
	return S_OK;
}

STDMETHODIMP TsfTextStore::GetStatus(TS_STATUS *pdcs) {
	if (!pdcs) return E_INVALIDARG;

	pdcs->dwDynamicFlags = 0;
	// The document is short lived, it only contains the current composition
	pdcs->dwStaticFlags = TS_SS_TRANSITORY | TS_SS_NOHIDDENTEXT;
	return S_OK;
}

STDMETHODIMP TsfTextStore::QueryInsert(LONG acpTestStart, LONG acpTestEnd, ULONG cch, LONG *pacpResultStart,
	LONG *pacpResultEnd) {
	if (!pacpResultStart || !pacpResultEnd) return E_INVALIDARG;
	if (acpTestStart < 0 || acpTestStart > acpTestEnd || acpTestEnd > text_length) return E_INVALIDARG;

	*pacpResultStart = acpTestStart;
	*pacpResultEnd = acpTestEnd;
	return S_OK;
}

STDMETHODIMP TsfTextStore::GetSelection(ULONG ulIndex, ULONG ulCount, TS_SELECTION_ACP *pSelection, ULONG *pcFetched) {
	if (!pSelection || !pcFetched) return E_INVALIDARG;
	if (!HasReadLock()) return TS_E_NOLOCK;

	*pcFetched = 0;
	if (ulCount > 0 && (ulIndex == 0 || ulIndex == TS_DEFAULT_SELECTION)) {
		pSelection[0].acpStart = selection_start;
		pSelection[0].acpEnd = selection_end;
		pSelection[0].style.ase = TS_AE_END;
		pSelection[0].style.fInterimChar = FALSE;
		*pcFetched = 1;
	}
	return S_OK;
}

STDMETHODIMP TsfTextStore::SetSelection(ULONG ulCount, const TS_SELECTION_ACP *pSelection) {
	if (!pSelection) return E_INVALIDARG;
	if (!HasReadWriteLock()) return TS_E_NOLOCK;

	if (ulCount > 0) {
		LONG start = pSelection[0].acpStart;
		LONG end = pSelection[0].acpEnd;
		if (start < 0 || start > end || end > text_length) return TS_E_INVALIDPOS;

		selection_start = start;
		selection_end = end;
		composition_dirty = true;
	}
	return S_OK;
}

STDMETHODIMP TsfTextStore::GetText(LONG acpStart, LONG acpEnd, WCHAR *pchPlain, ULONG cchPlainReq, ULONG *pcchPlainRet,
	TS_RUNINFO *prgRunInfo, ULONG cRunInfoReq, ULONG *pcRunInfoRet, LONG *pacpNext) {
	if (!pcchPlainRet || !pcRunInfoRet || !pacpNext) return E_INVALIDARG;
	if (!HasReadLock()) return TS_E_NOLOCK;

	*pcchPlainRet = 0;
	*pcRunInfoRet = 0;
	*pacpNext = acpStart;

	if (acpEnd == -1 || acpEnd > text_length) acpEnd = text_length;
	if (acpStart < 0 || acpStart > acpEnd) return TS_E_INVALIDPOS;

	ULONG count = static_cast<ULONG>(acpEnd - acpStart);
	if (cchPlainReq > 0) {
		count = min(count, cchPlainReq);
		if (pchPlain && count > 0) {
			memcpy(pchPlain, text + acpStart, count * sizeof(wchar_t));
		}
		*pcchPlainRet = count;
	}

	if (cRunInfoReq > 0 && prgRunInfo && count > 0) {
		prgRunInfo[0].uCount = count;
		prgRunInfo[0].type = TS_RT_PLAIN;
		*pcRunInfoRet = 1;
	}

	*pacpNext = acpStart + static_cast<LONG>(count);
	return S_OK;
}

STDMETHODIMP TsfTextStore::SetText(DWORD dwFlags, LONG acpStart, LONG acpEnd, const WCHAR *pchText, ULONG cch,
	TS_TEXTCHANGE *pChange) {
	if (!HasReadWriteLock()) return TS_E_NOLOCK;
	if (acpStart < 0 || acpStart > acpEnd || acpEnd > text_length) return TS_E_INVALIDPOS;
	if (!pchText && cch > 0) return E_INVALIDARG;

	if (!ReplaceText(acpStart, acpEnd, pchText, cch)) return E_OUTOFMEMORY;

	if (pChange) {
		pChange->acpStart = acpStart;
		pChange->acpOldEnd = acpEnd;
		pChange->acpNewEnd = acpStart + static_cast<LONG>(cch);
	}

	selection_start = acpStart + static_cast<LONG>(cch);
	selection_end = selection_start;
	return S_OK;
}

STDMETHODIMP TsfTextStore::GetFormattedText(LONG acpStart, LONG acpEnd, IDataObject **ppDataObject) {
	return E_NOTIMPL;
}

STDMETHODIMP TsfTextStore::GetEmbedded(LONG acpPos, REFGUID rguidService, REFIID riid, IUnknown **ppunk) {
	return E_NOTIMPL;
}

STDMETHODIMP TsfTextStore::QueryInsertEmbedded(const GUID *pguidService, const FORMATETC *pFormatEtc, BOOL *pfInsertable) {
	if (!pfInsertable) return E_INVALIDARG;

	*pfInsertable = FALSE;
	return S_OK;
}

STDMETHODIMP TsfTextStore::InsertEmbedded(DWORD dwFlags, LONG acpStart, LONG acpEnd, IDataObject *pDataObject,
	TS_TEXTCHANGE *pChange) {
	return E_NOTIMPL;
}

STDMETHODIMP TsfTextStore::InsertTextAtSelection(DWORD dwFlags, const WCHAR *pchText, ULONG cch, LONG *pacpStart,
	LONG *pacpEnd, TS_TEXTCHANGE *pChange) {
	LONG start = min(selection_start, selection_end);
	LONG end = max(selection_start, selection_end);

	if (dwFlags & TS_IAS_QUERYONLY) {
		if (!HasReadLock()) return TS_E_NOLOCK;
		if (pacpStart) *pacpStart = start;
		if (pacpEnd) *pacpEnd = end;
		return S_OK;
	}

	if (!HasReadWriteLock()) return TS_E_NOLOCK;
	if (!pchText && cch > 0) return E_INVALIDARG;

	if (!ReplaceText(start, end, pchText, cch)) return E_OUTOFMEMORY;

	LONG new_end = start + static_cast<LONG>(cch);
	if (!(dwFlags & TS_IAS_NOQUERY)) {
		if (pacpStart) *pacpStart = start;
		if (pacpEnd) *pacpEnd = new_end;
	}
	if (pChange) {
		pChange->acpStart = start;
		pChange->acpOldEnd = end;
		pChange->acpNewEnd = new_end;
	}

	selection_start = new_end;
	selection_end = new_end;
	return S_OK;
}

STDMETHODIMP TsfTextStore::InsertEmbeddedAtSelection(DWORD dwFlags, IDataObject *pDataObject, LONG *pacpStart,
	LONG *pacpEnd, TS_TEXTCHANGE *pChange) {
	return E_NOTIMPL;
}

void TsfTextStore::StoreRequestedAttrs(ULONG count, const TS_ATTRID *attrs) {
	requested_attr_count = 0;
	if (!attrs) return;

	for (ULONG i = 0; i < count && i < MAX_REQUESTED_ATTRS; ++i) {
		requested_attrs[requested_attr_count++] = attrs[i];
	}
}

STDMETHODIMP TsfTextStore::RequestSupportedAttrs(DWORD dwFlags, ULONG cFilterAttrs, const TS_ATTRID *paFilterAttrs) {
	// No attributes are supported, all requested attributes are reported as VT_EMPTY
	StoreRequestedAttrs(cFilterAttrs, paFilterAttrs);
	return S_OK;
}

STDMETHODIMP TsfTextStore::RequestAttrsAtPosition(LONG acpPos, ULONG cFilterAttrs, const TS_ATTRID *paFilterAttrs,
	DWORD dwFlags) {
	StoreRequestedAttrs(cFilterAttrs, paFilterAttrs);
	return S_OK;
}

STDMETHODIMP TsfTextStore::RequestAttrsTransitioningAtPosition(LONG acpPos, ULONG cFilterAttrs,
	const TS_ATTRID *paFilterAttrs, DWORD dwFlags) {
	requested_attr_count = 0;
	return S_OK;
}

STDMETHODIMP TsfTextStore::FindNextAttrTransition(LONG acpStart, LONG acpHalt, ULONG cFilterAttrs,
	const TS_ATTRID *paFilterAttrs, DWORD dwFlags, LONG *pacpNext, BOOL *pfFound, LONG *plFoundOffset) {
	if (!pacpNext || !pfFound || !plFoundOffset) return E_INVALIDARG;

	*pacpNext = acpHalt;
	*pfFound = FALSE;
	*plFoundOffset = 0;
	return S_OK;
}

STDMETHODIMP TsfTextStore::RetrieveRequestedAttrs(ULONG ulCount, TS_ATTRVAL *paAttrVals, ULONG *pcFetched) {
	if (!paAttrVals || !pcFetched) return E_INVALIDARG;

	ULONG count = min(ulCount, requested_attr_count);
	for (ULONG i = 0; i < count; ++i) {
		paAttrVals[i].idAttr = requested_attrs[i];
		paAttrVals[i].dwOverlapId = 0;
		VariantInit(&paAttrVals[i].varValue);
	}
	*pcFetched = count;
	requested_attr_count = 0;
	return S_OK;
}

STDMETHODIMP TsfTextStore::GetEndACP(LONG *pacp) {
	if (!pacp) return E_INVALIDARG;
	if (!HasReadLock()) return TS_E_NOLOCK;

	*pacp = text_length;
	return S_OK;
}

STDMETHODIMP TsfTextStore::GetActiveView(TsViewCookie *pvcView) {
	if (!pvcView) return E_INVALIDARG;

	*pvcView = TSF_VIEW_COOKIE;
	return S_OK;
}

STDMETHODIMP TsfTextStore::GetACPFromPoint(TsViewCookie vcView, const POINT *ptScreen, DWORD dwFlags, LONG *pacp) {
	return E_NOTIMPL;
}

STDMETHODIMP TsfTextStore::GetTextExt(TsViewCookie vcView, LONG acpStart, LONG acpEnd, RECT *prc, BOOL *pfClipped) {
	if (!prc || !pfClipped) return E_INVALIDARG;
	if (vcView != TSF_VIEW_COOKIE) return E_INVALIDARG;
	if (!HasReadLock()) return TS_E_NOLOCK;
	if (acpStart < 0 || acpStart > acpEnd || acpEnd > text_length) return TS_E_INVALIDPOS;

	*prc = RendererGetCompositionTextRect(renderer, text, static_cast<uint32_t>(text_length),
		static_cast<uint32_t>(acpStart), static_cast<uint32_t>(acpEnd));
	MapWindowPoints(hwnd, HWND_DESKTOP, reinterpret_cast<POINT *>(prc), 2);
	*pfClipped = FALSE;
	return S_OK;
}

STDMETHODIMP TsfTextStore::GetScreenExt(TsViewCookie vcView, RECT *prc) {
	if (!prc) return E_INVALIDARG;
	if (vcView != TSF_VIEW_COOKIE) return E_INVALIDARG;

	GetClientRect(hwnd, prc);
	MapWindowPoints(hwnd, HWND_DESKTOP, reinterpret_cast<POINT *>(prc), 2);
	return S_OK;
}

STDMETHODIMP TsfTextStore::GetWnd(TsViewCookie vcView, HWND *phwnd) {
	if (!phwnd) return E_INVALIDARG;
	if (vcView != TSF_VIEW_COOKIE) return E_INVALIDARG;

	*phwnd = hwnd;
	return S_OK;
}

//
// ITfContextOwnerCompositionSink
//

void TsfTextStore::SetCompositionView(ITfCompositionView *view) {
	if (view) {
		view->AddRef();
	}
	SafeRelease(&composition_view);
	composition_view = view;
	composition_dirty = true;
}

STDMETHODIMP TsfTextStore::OnStartComposition(ITfCompositionView *pComposition, BOOL *pfOk) {
	if (!pfOk) return E_INVALIDARG;

	SetCompositionView(pComposition);
	*pfOk = TRUE;
	return S_OK;
}

STDMETHODIMP TsfTextStore::OnUpdateComposition(ITfCompositionView *pComposition, ITfRange *pRangeNew) {
	SetCompositionView(pComposition);
	return S_OK;
}

STDMETHODIMP TsfTextStore::OnEndComposition(ITfCompositionView *pComposition) {
	SetCompositionView(nullptr);
	clause_count = 0;

	// Compositions can be ended outside of a lock (e.g. on focus changes),
	// make sure the result still makes it to nvim
	if (!lock_flags) {
		PostMessage(hwnd, WM_TSF_PROCESS_PENDING, 0, 0);
	}
	return S_OK;
}

//
// ITfTextEditSink
//

STDMETHODIMP TsfTextStore::OnEndEdit(ITfContext *pic, TfEditCookie ecReadOnly, ITfEditRecord *pEditRecord) {
	UpdateClauses(pic, ecReadOnly);
	composition_dirty = true;
	return S_OK;
}

// Converts a TSF color to 0xRRGGBB, returns false if the IME left it unspecified
static bool ConvertDisplayAttributeColor(const TF_DA_COLOR &color, uint32_t *rgb) {
	COLORREF color_ref;
	switch (color.type) {
	case TF_CT_SYSCOLOR: {
		color_ref = GetSysColor(color.nIndex);
	} break;
	case TF_CT_COLORREF: {
		color_ref = color.cr;
	} break;
	default: {
	} return false;
	}

	*rgb = (GetRValue(color_ref) << 16) | (GetGValue(color_ref) << 8) | GetBValue(color_ref);
	return true;
}

static CompositionLineStyle ConvertDisplayAttributeLineStyle(TF_DA_LINESTYLE line_style) {
	switch (line_style) {
	case TF_LS_SOLID: return CompositionLineStyle::Solid;
	case TF_LS_DOT: return CompositionLineStyle::Dot;
	case TF_LS_DASH: return CompositionLineStyle::Dash;
	case TF_LS_SQUIGGLE: return CompositionLineStyle::Squiggle;
	default: return CompositionLineStyle::None;
	}
}

// Collects the display attribute properties: GUID_PROP_ATTRIBUTE plus any
// property registered by text services under GUID_TFCAT_DISPLAYATTRIBUTEPROPERTY
void TsfTextStore::CollectDisplayAttributeProperties() {
	display_attribute_property_count = 0;
	display_attribute_properties[display_attribute_property_count++] = GUID_PROP_ATTRIBUTE;
	if (!category_mgr) return;

	IEnumGUID *enum_guids;
	if (SUCCEEDED(category_mgr->EnumItemsInCategory(GUID_TFCAT_DISPLAYATTRIBUTEPROPERTY, &enum_guids))) {
		GUID guid;
		while (display_attribute_property_count < MAX_DISPLAY_ATTRIBUTE_PROPERTIES &&
			enum_guids->Next(1, &guid, nullptr) == S_OK) {
			if (!IsEqualGUID(guid, GUID_PROP_ATTRIBUTE)) {
				display_attribute_properties[display_attribute_property_count++] = guid;
			}
		}
		enum_guids->Release();
	}
}

// Resolves a display attribute guid atom to its attribute info
bool TsfTextStore::GetDisplayAttribute(TfGuidAtom guid_atom, TF_DISPLAYATTRIBUTE *attribute) {
	GUID guid;
	if (FAILED(category_mgr->GetGUID(guid_atom, &guid))) return false;

	ITfDisplayAttributeInfo *info;
	if (FAILED(display_attribute_mgr->GetDisplayAttributeInfo(guid, &info, nullptr))) return false;

	bool success = SUCCEEDED(info->GetAttributeInfo(attribute));
	info->Release();
	return success;
}

// Reads the display attributes the IME applied to the composition: underline
// style and colors of each clause.
void TsfTextStore::UpdateClauses(ITfContext *pic, TfEditCookie ec) {
	clause_count = 0;
	if (!category_mgr || !display_attribute_mgr) return;

	const GUID *property_guids[MAX_DISPLAY_ATTRIBUTE_PROPERTIES];
	for (ULONG i = 0; i < display_attribute_property_count; ++i) {
		property_guids[i] = &display_attribute_properties[i];
	}

	ITfReadOnlyProperty *property = nullptr;
	ITfRange *range = nullptr;
	ITfRange *range_end = nullptr;
	IEnumTfRanges *enum_ranges = nullptr;

	if (SUCCEEDED(pic->TrackProperties(property_guids, display_attribute_property_count, nullptr, 0, &property)) &&
		SUCCEEDED(pic->GetStart(ec, &range)) &&
		SUCCEEDED(pic->GetEnd(ec, &range_end)) &&
		SUCCEEDED(range->ShiftEndToRange(ec, range_end, TF_ANCHOR_END)) &&
		SUCCEEDED(property->EnumRanges(ec, &enum_ranges, range))) {

		ITfRange *attribute_range;
		while (clause_count < MAX_COMPOSITION_CLAUSES && enum_ranges->Next(1, &attribute_range, nullptr) == S_OK) {
			TF_DISPLAYATTRIBUTE attribute;
			bool has_attribute = false;

			// A tracked property's value is an enumeration of the values of each tracked property,
			// the first one which resolves to a display attribute wins
			VARIANT value;
			VariantInit(&value);
			if (SUCCEEDED(property->GetValue(ec, attribute_range, &value))) {
				IEnumTfPropertyValue *enum_values;
				if (value.vt == VT_UNKNOWN && value.punkVal &&
					SUCCEEDED(value.punkVal->QueryInterface(IID_IEnumTfPropertyValue, reinterpret_cast<void **>(&enum_values)))) {
					TF_PROPERTYVAL property_value;
					while (!has_attribute && enum_values->Next(1, &property_value, nullptr) == S_OK) {
						if (property_value.varValue.vt == VT_I4 || property_value.varValue.vt == VT_UI4) {
							has_attribute = GetDisplayAttribute(static_cast<TfGuidAtom>(property_value.varValue.lVal), &attribute);
						}
						VariantClear(&property_value.varValue);
					}
					enum_values->Release();
				}
				else if (value.vt == VT_I4 || value.vt == VT_UI4) {
					has_attribute = GetDisplayAttribute(static_cast<TfGuidAtom>(value.lVal), &attribute);
				}
			}
			VariantClear(&value);

			ITfRangeACP *range_acp = nullptr;
			LONG start, length;
			if (has_attribute &&
				SUCCEEDED(attribute_range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void **>(&range_acp))) &&
				SUCCEEDED(range_acp->GetExtent(&start, &length)) && length > 0) {
				CompositionClause *clause = &clauses[clause_count++];
				*clause = CompositionClause {
					.start = static_cast<uint32_t>(start),
					.end = static_cast<uint32_t>(start + length),
					.line_style = ConvertDisplayAttributeLineStyle(attribute.lsStyle),
					.bold_line = attribute.fBoldLine != FALSE
				};
				clause->has_text_color = ConvertDisplayAttributeColor(attribute.crText, &clause->text_color);
				clause->has_background_color = ConvertDisplayAttributeColor(attribute.crBk, &clause->background_color);
				clause->has_line_color = ConvertDisplayAttributeColor(attribute.crLine, &clause->line_color);
			}
			SafeRelease(&range_acp);
			attribute_range->Release();
		}
	}

	SafeRelease(&enum_ranges);
	SafeRelease(&range_end);
	SafeRelease(&range);
	SafeRelease(&property);
}

//
// Helpers
//

bool TsfTextStore::ReplaceText(LONG start, LONG end, const wchar_t *new_text, ULONG new_length) {
	LONG new_text_length = text_length - (end - start) + static_cast<LONG>(new_length);
	if (new_text_length > text_capacity) {
		LONG new_capacity = max(new_text_length, max(64L, text_capacity * 2));
		wchar_t *new_buffer = static_cast<wchar_t *>(realloc(text, new_capacity * sizeof(wchar_t)));
		if (!new_buffer) return false;
		text = new_buffer;
		text_capacity = new_capacity;
	}

	memmove(text + start + new_length, text + end, (text_length - end) * sizeof(wchar_t));
	if (new_length > 0) {
		memcpy(text + start, new_text, new_length * sizeof(wchar_t));
	}
	text_length = new_text_length;
	composition_dirty = true;
	return true;
}

void TsfTextStore::RemoveTextPrefix(LONG count) {
	memmove(text, text + count, (text_length - count) * sizeof(wchar_t));
	text_length -= count;
	selection_start = max(0L, selection_start - count);
	selection_end = max(0L, selection_end - count);

	uint32_t remaining = 0;
	for (uint32_t i = 0; i < clause_count; ++i) {
		if (clauses[i].end <= static_cast<uint32_t>(count)) continue;
		clauses[remaining] = clauses[i];
		clauses[remaining].start = clauses[i].start > static_cast<uint32_t>(count) ?
			clauses[i].start - count : 0;
		clauses[remaining].end = clauses[i].end - count;
		++remaining;
	}
	clause_count = remaining;
	composition_dirty = true;
}

LONG TsfTextStore::GetCompositionStart() {
	LONG start = 0;
	ITfRange *range;
	if (SUCCEEDED(composition_view->GetRange(&range))) {
		ITfRangeACP *range_acp;
		if (SUCCEEDED(range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void **>(&range_acp)))) {
			LONG length;
			if (FAILED(range_acp->GetExtent(&start, &length))) {
				start = 0;
			}
			range_acp->Release();
		}
		range->Release();
	}
	return max(0L, min(start, text_length));
}

// Called whenever no lock is held. Any text that is not part of the composition
// anymore has been committed by the IME, send it to nvim and drop it from the store.
void TsfTextStore::ProcessPending() {
	if (lock_flags || processing) return;
	processing = true;
	AddRef();

	if (layout_change_pending) {
		layout_change_pending = false;
		if (sink && (sink_mask & TS_AS_LAYOUT_CHANGE)) {
			sink->OnLayoutChange(TS_LC_CHANGE, TSF_VIEW_COOKIE);
		}
	}

	LONG commit_length = composition_view ? GetCompositionStart() : text_length;
	if (commit_length > 0) {
		NvimSendString(nvim, text, static_cast<size_t>(commit_length));
		RemoveTextPrefix(commit_length);

		if (sink && (sink_mask & TS_AS_TEXT_CHANGE)) {
			TS_TEXTCHANGE change {
				.acpStart = 0,
				.acpOldEnd = commit_length,
				.acpNewEnd = 0
			};
			sink->OnTextChange(0, &change);
		}
		if (sink && (sink_mask & TS_AS_SEL_CHANGE)) {
			sink->OnSelectionChange();
		}
	}

	if (composition_dirty) {
		composition_dirty = false;
		if (composition_view && text_length > 0) {
			RendererSetComposition(renderer, text, static_cast<uint32_t>(text_length),
				static_cast<uint32_t>(selection_end), clauses, clause_count);
		}
		else {
			RendererSetComposition(renderer, nullptr, 0, 0, nullptr, 0);
		}
	}

	processing = false;

	// Notifying the sink may have granted new locks while we were processing,
	// handle any changes made during those once we're back in the message loop
	if (locked_while_processing) {
		locked_while_processing = false;
		PostMessage(hwnd, WM_TSF_PROCESS_PENDING, 0, 0);
	}
	Release();
}

void TsfTextStore::NotifyLayoutChange() {
	layout_change_pending = true;
	ProcessPending();
}

void TsfTextStore::TerminateComposition() {
	if (!composition_view || !context) return;

	AddRef();
	ITfContextOwnerCompositionServices *composition_services;
	if (SUCCEEDED(context->QueryInterface(IID_ITfContextOwnerCompositionServices,
		reinterpret_cast<void **>(&composition_services)))) {
		composition_services->TerminateComposition(composition_view);
		composition_services->Release();
	}
	ProcessPending();
	Release();
}

//
// Public interface
//

bool TsfInitialize(Tsf *tsf, HWND hwnd, Renderer *renderer, Nvim *nvim) {
	tsf->hwnd = hwnd;

	if (FAILED(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
		reinterpret_cast<void **>(&tsf->thread_mgr)))) {
		tsf->thread_mgr = nullptr;
		return false;
	}

	if (FAILED(tsf->thread_mgr->Activate(&tsf->client_id))) {
		SafeRelease(&tsf->thread_mgr);
		return false;
	}

	tsf->text_store = new TsfTextStore(hwnd, renderer, nvim);
	if (FAILED(tsf->thread_mgr->CreateDocumentMgr(&tsf->document_mgr)) ||
		FAILED(tsf->document_mgr->CreateContext(tsf->client_id, 0, static_cast<ITextStoreACP *>(tsf->text_store),
			&tsf->context, &tsf->edit_cookie)) ||
		FAILED(tsf->document_mgr->Push(tsf->context))) {
		TsfShutdown(tsf);
		return false;
	}

	tsf->text_store->Attach(tsf->context);

	// Let TSF switch focus to our document whenever the window gains focus
	ITfDocumentMgr *previous_document_mgr = nullptr;
	tsf->thread_mgr->AssociateFocus(hwnd, tsf->document_mgr, &previous_document_mgr);
	SafeRelease(&previous_document_mgr);

	if (GetFocus() == hwnd) {
		tsf->thread_mgr->SetFocus(tsf->document_mgr);
	}

	return true;
}

void TsfShutdown(Tsf *tsf) {
	if (tsf->text_store) {
		tsf->text_store->Detach();
	}

	if (tsf->thread_mgr && tsf->document_mgr) {
		ITfDocumentMgr *previous_document_mgr = nullptr;
		tsf->thread_mgr->AssociateFocus(tsf->hwnd, nullptr, &previous_document_mgr);
		SafeRelease(&previous_document_mgr);
	}

	if (tsf->document_mgr) {
		tsf->document_mgr->Pop(TF_POPF_ALL);
	}

	SafeRelease(&tsf->context);
	SafeRelease(&tsf->document_mgr);
	SafeRelease(&tsf->text_store);

	if (tsf->thread_mgr) {
		tsf->thread_mgr->Deactivate();
		SafeRelease(&tsf->thread_mgr);
	}
}

void TsfNotifyLayoutChange(Tsf *tsf) {
	if (tsf->text_store) {
		tsf->text_store->NotifyLayoutChange();
	}
}

void TsfTerminateComposition(Tsf *tsf) {
	if (tsf->text_store) {
		tsf->text_store->TerminateComposition();
	}
}

bool TsfIsComposing(Tsf *tsf) {
	return tsf->text_store && tsf->text_store->IsComposing();
}

void TsfProcessPending(Tsf *tsf) {
	if (tsf->text_store) {
		tsf->text_store->ProcessPending();
	}
}
