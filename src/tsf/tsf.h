#pragma once

struct Renderer;
struct Nvim;
class TsfTextStore;

struct Tsf {
	HWND hwnd;
	ITfThreadMgr *thread_mgr;
	TfClientId client_id;
	ITfDocumentMgr *document_mgr;
	ITfContext *context;
	TfEditCookie edit_cookie;
	TsfTextStore *text_store;
};

bool TsfInitialize(Tsf *tsf, HWND hwnd, Renderer *renderer, Nvim *nvim);
void TsfShutdown(Tsf *tsf);

// Notify the text services that the on-screen position of the text changed
// (cursor moved, font changed, window moved/resized...)
void TsfNotifyLayoutChange(Tsf *tsf);
// Commits the current composition (if any) and sends it to nvim
void TsfTerminateComposition(Tsf *tsf);
bool TsfIsComposing(Tsf *tsf);
// Handles WM_TSF_PROCESS_PENDING
void TsfProcessPending(Tsf *tsf);
