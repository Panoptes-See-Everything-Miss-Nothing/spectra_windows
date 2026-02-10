#include "InstalledUpdates.h"
#include "Utils/Utils.h"
#include <wuapi.h>
#include <comdef.h>
#include <oleauto.h>
#include <unordered_map>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "wuguid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// RAII COM initializer scoped to this translation unit.
// WUA COM objects are apartment-threaded; MTA is used because the
// worker thread has no message pump and other collectors already use MTA.
namespace
{
    class WuaComScope
    {
    public:
        WuaComScope() : m_initialized(false)
        {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(hr))
            {
                m_initialized = true;
            }
            else if (hr == RPC_E_CHANGED_MODE)
            {
                // COM already initialized in a compatible mode — that's fine,
                // but we must NOT call CoUninitialize in our destructor.
                m_initialized = false;
            }
            else
            {
                LogError("[-] InstalledUpdates: CoInitializeEx failed, HRESULT: 0x" +
                         std::to_string(static_cast<unsigned long>(hr)));
            }
        }

        ~WuaComScope()
        {
            if (m_initialized)
            {
                CoUninitialize();
            }
        }

        WuaComScope(const WuaComScope&) = delete;
        WuaComScope& operator=(const WuaComScope&) = delete;

    private:
        bool m_initialized;
    };

    // RAII wrapper for BSTR
    class BstrGuard
    {
    public:
        explicit BstrGuard(BSTR bstr) : m_bstr(bstr) {}
        ~BstrGuard() { if (m_bstr) SysFreeString(m_bstr); }

        BstrGuard(const BstrGuard&) = delete;
        BstrGuard& operator=(const BstrGuard&) = delete;

        const wchar_t* Get() const { return m_bstr ? m_bstr : L""; }
        BSTR Raw() const { return m_bstr; }

    private:
        BSTR m_bstr;
    };

    // Safe BSTR-to-wstring extraction
    std::wstring BstrToWstring(BSTR bstr)
    {
        if (!bstr) return {};
        return std::wstring(bstr, SysStringLen(bstr));
    }
} // anonymous namespace

std::wstring InstalledUpdatesCollector::DateToIso8601(double variantDate)
{
    if (variantDate == 0.0)
        return {};

    SYSTEMTIME st = {};
    if (!VariantTimeToSystemTime(variantDate, &st))
        return {};

    wchar_t buffer[32] = {};
    _snwprintf_s(buffer, _countof(buffer), _TRUNCATE,
                 L"%04u-%02u-%02uT%02u:%02u:%02u",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);

    return buffer;
}

bool InstalledUpdatesCollector::QueryInstalledUpdates(std::vector<InstalledUpdate>& updates)
{
    // Create update session
    IUpdateSession* pSession = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(UpdateSession), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IUpdateSession), reinterpret_cast<void**>(&pSession));

    if (FAILED(hr) || !pSession)
    {
        LogError("[-] InstalledUpdates: Failed to create IUpdateSession, HRESULT: 0x" +
                 std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }

    // Create update searcher
    IUpdateSearcher* pSearcher = nullptr;
    hr = pSession->CreateUpdateSearcher(&pSearcher);
    if (FAILED(hr) || !pSearcher)
    {
        LogError("[-] InstalledUpdates: Failed to create IUpdateSearcher, HRESULT: 0x" +
                 std::to_string(static_cast<unsigned long>(hr)));
        pSession->Release();
        return false;
    }

    LogError("[+] InstalledUpdates: Searching for installed updates (IsInstalled=1)...");

    // Search for installed updates — this queries the local WUA datastore only,
    // no network/WSUS connectivity is required.
    BSTR criteria = SysAllocString(L"IsInstalled=1");
    if (!criteria)
    {
        LogError("[-] InstalledUpdates: SysAllocString failed for search criteria");
        pSearcher->Release();
        pSession->Release();
        return false;
    }

    ISearchResult* pResult = nullptr;
    hr = pSearcher->Search(criteria, &pResult);
    SysFreeString(criteria);

    if (FAILED(hr) || !pResult)
    {
        LogError("[-] InstalledUpdates: Search failed, HRESULT: 0x" +
                 std::to_string(static_cast<unsigned long>(hr)));
        pSearcher->Release();
        pSession->Release();
        return false;
    }

    // Get update collection from results
    IUpdateCollection* pUpdates = nullptr;
    hr = pResult->get_Updates(&pUpdates);
    if (FAILED(hr) || !pUpdates)
    {
        LogError("[-] InstalledUpdates: Failed to get update collection, HRESULT: 0x" +
                 std::to_string(static_cast<unsigned long>(hr)));
        pResult->Release();
        pSearcher->Release();
        pSession->Release();
        return false;
    }

    LONG count = 0;
    pUpdates->get_Count(&count);

    LogError("[+] InstalledUpdates: Found " + std::to_string(count) + " installed updates");

    updates.reserve(static_cast<size_t>(count));

    for (LONG i = 0; i < count; ++i)
    {
        IUpdate* pUpdate = nullptr;
        hr = pUpdates->get_Item(i, &pUpdate);
        if (FAILED(hr) || !pUpdate)
            continue;

        InstalledUpdate entry{};

        // Title
        {
            BSTR bstr = nullptr;
            if (SUCCEEDED(pUpdate->get_Title(&bstr)))
            {
                entry.title = BstrToWstring(bstr);
                SysFreeString(bstr);
            }
        }

        // Description
        {
            BSTR bstr = nullptr;
            if (SUCCEEDED(pUpdate->get_Description(&bstr)))
            {
                entry.description = BstrToWstring(bstr);
                SysFreeString(bstr);
            }
        }

        // Support URL
        {
            BSTR bstr = nullptr;
            if (SUCCEEDED(pUpdate->get_SupportUrl(&bstr)))
            {
                entry.supportUrl = BstrToWstring(bstr);
                SysFreeString(bstr);
            }
        }

        // Update Identity (GUID + revision)
        {
            IUpdateIdentity* pIdentity = nullptr;
            if (SUCCEEDED(pUpdate->get_Identity(&pIdentity)) && pIdentity)
            {
                BSTR bstr = nullptr;
                if (SUCCEEDED(pIdentity->get_UpdateID(&bstr)))
                {
                    entry.updateId = BstrToWstring(bstr);
                    SysFreeString(bstr);
                }

                LONG rev = 0;
                if (SUCCEEDED(pIdentity->get_RevisionNumber(&rev)))
                {
                    entry.revisionNumber = static_cast<int>(rev);
                }

                pIdentity->Release();
            }
        }

        // KB Article IDs
        {
            IStringCollection* pKBs = nullptr;
            if (SUCCEEDED(pUpdate->get_KBArticleIDs(&pKBs)) && pKBs)
            {
                LONG kbCount = 0;
                pKBs->get_Count(&kbCount);
                for (LONG k = 0; k < kbCount; ++k)
                {
                    BSTR bstr = nullptr;
                    if (SUCCEEDED(pKBs->get_Item(k, &bstr)))
                    {
                        std::wstring kb = BstrToWstring(bstr);
                        if (!kb.empty())
                            entry.kbArticleIds.push_back(std::move(kb));
                        SysFreeString(bstr);
                    }
                }
                pKBs->Release();
            }
        }

        // Categories
        {
            ICategoryCollection* pCats = nullptr;
            if (SUCCEEDED(pUpdate->get_Categories(&pCats)) && pCats)
            {
                LONG catCount = 0;
                pCats->get_Count(&catCount);
                for (LONG c = 0; c < catCount; ++c)
                {
                    ICategory* pCat = nullptr;
                    if (SUCCEEDED(pCats->get_Item(c, &pCat)) && pCat)
                    {
                        BSTR bstr = nullptr;
                        if (SUCCEEDED(pCat->get_Name(&bstr)))
                        {
                            std::wstring catName = BstrToWstring(bstr);
                            if (!catName.empty())
                                entry.categories.push_back(std::move(catName));
                            SysFreeString(bstr);
                        }
                        pCat->Release();
                    }
                }
                pCats->Release();
            }
        }

        // MSRC Severity — available on IUpdate3+
        {
            IUpdate3* pUpdate3 = nullptr;
            hr = pUpdate->QueryInterface(__uuidof(IUpdate3), reinterpret_cast<void**>(&pUpdate3));
            if (SUCCEEDED(hr) && pUpdate3)
            {
                BSTR bstr = nullptr;
                if (SUCCEEDED(pUpdate3->get_MsrcSeverity(&bstr)))
                {
                    entry.msrcSeverity = BstrToWstring(bstr);
                    SysFreeString(bstr);
                }
                pUpdate3->Release();
            }
        }

        pUpdate->Release();
        updates.push_back(std::move(entry));
    }

    pUpdates->Release();
    pResult->Release();
    pSearcher->Release();
    pSession->Release();

    return true;
}

void InstalledUpdatesCollector::EnrichWithHistory(std::vector<InstalledUpdate>& updates)
{
    if (updates.empty())
        return;

    // Create a lookup map: updateId -> index in updates vector
    std::unordered_map<std::wstring, size_t> idToIndex;
    for (size_t i = 0; i < updates.size(); ++i)
    {
        if (!updates[i].updateId.empty())
        {
            idToIndex[updates[i].updateId] = i;
        }
    }

    IUpdateSession* pSession = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(UpdateSession), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IUpdateSession), reinterpret_cast<void**>(&pSession));

    if (FAILED(hr) || !pSession)
    {
        LogError("[!] InstalledUpdates: Could not create session for history enrichment");
        return;
    }

    IUpdateSearcher* pSearcher = nullptr;
    hr = pSession->CreateUpdateSearcher(&pSearcher);
    if (FAILED(hr) || !pSearcher)
    {
        pSession->Release();
        return;
    }

    LONG totalHistory = 0;
    hr = pSearcher->GetTotalHistoryCount(&totalHistory);
    if (FAILED(hr) || totalHistory <= 0)
    {
        pSearcher->Release();
        pSession->Release();
        return;
    }

    IUpdateHistoryEntryCollection* pHistoryCollection = nullptr;
    hr = pSearcher->QueryHistory(0, totalHistory, &pHistoryCollection);
    if (FAILED(hr) || !pHistoryCollection)
    {
        pSearcher->Release();
        pSession->Release();
        return;
    }

    LONG histCount = 0;
    pHistoryCollection->get_Count(&histCount);

    for (LONG i = 0; i < histCount; ++i)
    {
        IUpdateHistoryEntry* pEntry = nullptr;
        if (FAILED(pHistoryCollection->get_Item(i, &pEntry)) || !pEntry)
            continue;

        // Get the update identity from the history entry
        IUpdateIdentity* pIdentity = nullptr;
        if (SUCCEEDED(pEntry->get_UpdateIdentity(&pIdentity)) && pIdentity)
        {
            BSTR bstrId = nullptr;
            if (SUCCEEDED(pIdentity->get_UpdateID(&bstrId)))
            {
                std::wstring histUpdateId = BstrToWstring(bstrId);
                SysFreeString(bstrId);

                auto it = idToIndex.find(histUpdateId);
                if (it != idToIndex.end())
                {
                    InstalledUpdate& target = updates[it->second];

                    // Only overwrite if we haven't already set a date
                    if (target.installedDate.empty())
                    {
                        DATE date = 0.0;
                        if (SUCCEEDED(pEntry->get_Date(&date)))
                        {
                            target.installedDate = DateToIso8601(date);
                        }
                    }

                    // Operation result code
                    OperationResultCode resultCode = orcNotStarted;
                    if (SUCCEEDED(pEntry->get_ResultCode(&resultCode)))
                    {
                        target.operationResultCode = static_cast<int>(resultCode);
                    }
                }
            }
            pIdentity->Release();
        }

        pEntry->Release();
    }

    pHistoryCollection->Release();
    pSearcher->Release();
    pSession->Release();

    LogError("[+] InstalledUpdates: Enriched updates with installation history");
}

std::vector<InstalledUpdate> InstalledUpdatesCollector::Collect()
{
    LogError("[+] InstalledUpdates: Starting installed updates enumeration...");

    WuaComScope comScope;

    std::vector<InstalledUpdate> updates;

    if (!QueryInstalledUpdates(updates))
    {
        LogError("[-] InstalledUpdates: Failed to query installed updates");
        return {};
    }

    // Enrich with installation dates from update history
    EnrichWithHistory(updates);

    LogError("[+] InstalledUpdates: Enumeration complete — " +
             std::to_string(updates.size()) + " installed updates collected");

    return updates;
}
