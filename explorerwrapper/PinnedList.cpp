#include "PinnedList.h"
#include "dbgprint.h"
#include "OSVersion.h"

CPinnedListWrapper::CPinnedListWrapper(IUnknown* flex, int build)
{
	m_build = build;
	if (build >= 10240 && build < 14393)
	{
		m_pinnedList25 = (IPinnedList25*)flex;
		dbgprintf(L"using IPinnedList25");
	}
	else if (build >= 14393 && build < 17763)
	{
		m_flexList = (IFlexibleTaskbarPinnedList*)flex;
		dbgprintf(L"using IFlexibleTaskbarPinnedList");
	}
	else if (build >= 17763)
	{
		m_pinnedList3 = (IPinnedList3*)flex;
		dbgprintf(L"using IPinnedlist3");
	}
}

CPinnedListWrapper::~CPinnedListWrapper()
{
	if (m_pinnedList25)
		m_pinnedList25->Release();
	if (m_flexList)
		m_flexList->Release();
	if (m_pinnedList3)
		m_pinnedList3->Release();
}

HRESULT __stdcall CPinnedListWrapper::QueryInterface(REFIID riid, void** ppvObject)
{
	if (m_pinnedList25)
		return m_pinnedList25->QueryInterface(riid, ppvObject);
	if (m_flexList)
		return m_flexList->QueryInterface(riid, ppvObject);
	if (m_pinnedList3)
		return m_pinnedList3->QueryInterface(riid, ppvObject);
	return S_OK;
}

ULONG __stdcall CPinnedListWrapper::AddRef(void)
{
	ULONG cref;
	if (m_pinnedList25)
		cref = m_pinnedList25->AddRef();
	if (m_flexList)
		cref = m_flexList->AddRef();
	if (m_pinnedList3)
		cref = m_pinnedList3->AddRef();
	return cref;
}

ULONG __stdcall CPinnedListWrapper::Release(void)
{
	ULONG cref;
	if (m_pinnedList25)
		cref = m_pinnedList25->Release();
	if (m_flexList)
		cref = m_flexList->Release();
	if (m_pinnedList3)
		cref = m_pinnedList3->Release();
	if (cref == 0)
		free((void*)this);
	return cref;
}

//.text:00007FF6BEB24439 explorer.exe:$94439 #93A39
HRESULT __stdcall CPinnedListWrapper::EnumObjects(IEnumFullIDList** p1)
{
	if (m_pinnedList25)
		return m_pinnedList25->EnumObjects(p1);
	if (m_flexList)
		return m_flexList->EnumObjects(p1);
	if (m_pinnedList3)
		return m_pinnedList3->EnumObjects(p1);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::Modify(PCIDLIST_ABSOLUTE p1, PCIDLIST_ABSOLUTE p2)
{
	if (m_pinnedList25)
		return m_pinnedList25->Modify(p1, p2);
	if (m_flexList)
		return m_flexList->Modify(p1, p2);
	if (m_pinnedList3)
		return m_pinnedList3->Modify(p1, p2, 18);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::GetChangeCount(ULONG* p1)
{
	if (m_pinnedList25)
		return m_pinnedList25->GetChangeCount(p1);
	if (m_flexList)
		return m_flexList->GetChangeCount(p1);
	if (m_pinnedList3)
		return m_pinnedList3->GetChangeCount(p1);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::GetPinnableInfo(IDataObject* p1, int p2, IShellItem2** p3, IShellItem** p4, PWSTR* p5, INT* p6)
{
	if (!s_UseTaskbarPinning)
	{
		// in this situation, we don't take the information
		// this is to prevent potential issues arising with people trying to pin despite being in this mode
		return E_NOINTERFACE;
	}

	if (m_pinnedList25)
		return m_pinnedList25->GetPinnableInfo(p1, p2, p3, p4, p5, p6);
	if (m_flexList)
		return m_flexList->GetPinnableInfo(p1, p2, p3, p4, p5, p6);
	if (m_pinnedList3)
		return m_pinnedList3->GetPinnableInfo(p1, p2, p3, p4, p5, p6);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::IsPinnable(IDataObject* p1, int p2)
{
	if (m_pinnedList25)
		return m_pinnedList25->IsPinnable(p1, p2);
	if (m_flexList)
		return m_flexList->IsPinnable(p1, p2);
	if (m_pinnedList3)
		return m_pinnedList3->IsPinnable(p1, p2);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::Resolve(HWND p1, ULONG p2, PCIDLIST_ABSOLUTE p3, PIDLIST_ABSOLUTE* p4)
{
	if (m_pinnedList25)
		return m_pinnedList25->Resolve(p1, p2, p3, p4);
	if (m_flexList)
		return m_flexList->Resolve(p1, p2, p3, p4);
	if (m_pinnedList3)
		return m_pinnedList3->Resolve(p1, p2, p3, p4);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::IsPinned(PCIDLIST_ABSOLUTE p1)
{
	if (m_pinnedList25)
		return m_pinnedList25->IsPinned(p1);
	if (m_flexList)
		return m_flexList->IsPinned(p1);
	if (m_pinnedList3)
		return m_pinnedList3->IsPinned(p1);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::GetPinnedItem(PCIDLIST_ABSOLUTE p1, PIDLIST_ABSOLUTE* p2)
{
	if (m_pinnedList25)
		return m_pinnedList25->GetPinnedItem(p1, p2);
	if (m_flexList)
		return m_flexList->GetPinnedItem(p1, p2);
	if (m_pinnedList3)
		return m_pinnedList3->GetPinnedItem(p1, p2);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::GetAppIDForPinnedItem(PCIDLIST_ABSOLUTE p1, PWSTR* p2)
{
	// Ittr: Determine whether we should hide immersive items
	bool bHideImmersivePidl = false;

	// Only bother running the filtering code if the option to show store applications on the taskbar is disabled
	if (!s_ShowStoreAppsOnTaskbar)
	{
		ITEMIDLIST_ABSOLUTE* pidlApplicationFolder;
		if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, nullptr, &pidlApplicationFolder)))
		{
			bHideImmersivePidl = ILIsParent(pidlApplicationFolder, p1, TRUE);
		}
		CoTaskMemFree(pidlApplicationFolder);
	}

	// Cause the interface to intentionally fail if pinning is disabled or in the cases where immersive items should be hidden
	if (!s_UseTaskbarPinning || bHideImmersivePidl)
	{
		return E_NOINTERFACE;
	}

	// Pass through to the appropriate PinnedList interface for the user's version of Windows
	if (m_pinnedList25)
		return m_pinnedList25->GetAppIDForPinnedItem(p1, p2);
	if (m_flexList)
		return m_flexList->GetAppIDForPinnedItem(p1, p2);
	if (m_pinnedList3)
		return m_pinnedList3->GetAppIDForPinnedItem(p1, p2);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::ItemChangeNotify(PCIDLIST_ABSOLUTE p1, PCIDLIST_ABSOLUTE p2)
{
	if (m_pinnedList25)
		return m_pinnedList25->ItemChangeNotify(p1, p2);
	if (m_flexList)
		return m_flexList->ItemChangeNotify(p1, p2);
	if (m_pinnedList3)
		return m_pinnedList3->ItemChangeNotify(p1, p2);
	return S_OK;
}

HRESULT __stdcall CPinnedListWrapper::UpdateForRemovedItemsAsNecessary(VOID)
{
	if (m_pinnedList25)
		return m_pinnedList25->UpdateForRemovedItemsAsNecessary();
	if (m_flexList)
		return m_flexList->UpdateForRemovedItemsAsNecessary();
	if (m_pinnedList3)
		return m_pinnedList3->UpdateForRemovedItemsAsNecessary();
	return S_OK;
}

CPinnedListVistaWrapper::CPinnedListVistaWrapper(IUnknown* flex, int build)
{
	m_cRef = 1;
	m_inner = new CPinnedListWrapper(flex, build);
}

CPinnedListVistaWrapper::~CPinnedListVistaWrapper()
{
	if (m_inner)
	{
		m_inner->Release();
		m_inner = nullptr;
	}
}

HRESULT __stdcall CPinnedListVistaWrapper::QueryInterface(REFIID riid, void** ppvObject)
{
	if (!ppvObject)
		return E_POINTER;

	if (riid == IID_IUnknown || riid == IID_IPinnedListVista)
	{
		*ppvObject = static_cast<IPinnedListVista*>(this);
		AddRef();
		return S_OK;
	}

	return m_inner ? m_inner->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
}

ULONG __stdcall CPinnedListVistaWrapper::AddRef(void)
{
	return InterlockedIncrement(&m_cRef);
}

ULONG __stdcall CPinnedListVistaWrapper::Release(void)
{
	ULONG ref = InterlockedDecrement(&m_cRef);
	if (ref == 0)
	{
		delete this;
	}
	return ref;
}

HRESULT __stdcall CPinnedListVistaWrapper::EnumObjects(IEnumFullIDList** p1)
{
	return m_inner ? m_inner->EnumObjects(p1) : E_FAIL;
}

HRESULT __stdcall CPinnedListVistaWrapper::Modify(PCIDLIST_ABSOLUTE p1, PCIDLIST_ABSOLUTE p2)
{
	return m_inner ? m_inner->Modify(p1, p2) : E_FAIL;
}

HRESULT __stdcall CPinnedListVistaWrapper::GetChangeCount(ULONG* p1)
{
	return m_inner ? m_inner->GetChangeCount(p1) : E_FAIL;
}

HRESULT __stdcall CPinnedListVistaWrapper::IsPinnable(IDataObject* p1, int p2)
{
	return m_inner ? m_inner->IsPinnable(p1, p2) : E_FAIL;
}

HRESULT __stdcall CPinnedListVistaWrapper::Resolve(HWND p1, ULONG p2, PCIDLIST_ABSOLUTE p3, PIDLIST_ABSOLUTE* p4)
{
	return m_inner ? m_inner->Resolve(p1, p2, p3, p4) : E_FAIL;
}

HRESULT __stdcall CPinnedListVistaWrapper::IsPinned(PCIDLIST_ABSOLUTE p1)
{
	return m_inner ? m_inner->IsPinned(p1) : E_FAIL;
}

HRESULT __stdcall CPinnedListVistaWrapper::GetPinnedItem(PCIDLIST_ABSOLUTE p1, PIDLIST_ABSOLUTE* p2)
{
	return m_inner ? m_inner->GetPinnedItem(p1, p2) : E_FAIL;
}
