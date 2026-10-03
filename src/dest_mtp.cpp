// MTP destination via Windows Portable Devices (WPD).
// Destination syntax: mtp://<device name>/<storage name>[/folder/...], e.g. mtp://Pixel 7/Internal shared storage/Music

#include "dest_fs.h"
#include "sync_core.h"

#define NOMINMAX
#include <windows.h>
#include <PortableDeviceApi.h>
#include <PortableDevice.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <unordered_map>

#pragma comment(lib, "PortableDeviceGuids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

namespace psync {

	namespace {

		constexpr const wchar_t* kTempSuffix = L".psync-tmp";

		// Set PSYNC_MTP_TRACE=1 to print the duration of each device operation to stderr (diagnostics only).
		struct OpTimer {
			const wchar_t* what;
			ULONGLONG t0 = GetTickCount64();
			explicit OpTimer(const wchar_t* w) : what(w) {}
			~OpTimer() {
				static const bool on = GetEnvironmentVariableW(L"PSYNC_MTP_TRACE", nullptr, 0) > 0;
				if (on) fwprintf(stderr, L"[mtp] %-12s %5llu ms\n", what, GetTickCount64() - t0);
			}
		};

		void trace(const wchar_t* what, HRESULT hr) {
			static const bool on = GetEnvironmentVariableW(L"PSYNC_MTP_TRACE", nullptr, 0) > 0;
			if (on) fwprintf(stderr, L"[mtp] %s 0x%08lX\n", what, (unsigned long)hr);
		}

		std::wstring hrText(HRESULT hr) { return sysErrorText((unsigned long)hr); }

		template<typename T> ComPtr<T> create(REFCLSID clsid) {
			ComPtr<T> p;
			CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p));
			return p;
		}

		std::wstring takeString(LPWSTR s) {
			std::wstring r = s ? s : L"";
			CoTaskMemFree(s);
			return r;
		}

		bool sameName(const std::wstring& a, const std::wstring& b) { return pathKey(a) == pathKey(b); }

		std::vector<std::wstring> splitPath(const std::wstring& p) {
			std::vector<std::wstring> parts;
			size_t s = 0;
			while (s <= p.size()) {
				size_t t = p.find_first_of(L"/\\", s);
				if (t == std::wstring::npos) t = p.size();
				if (t > s) parts.push_back(p.substr(s, t - s));
				s = t + 1;
			}
			return parts;
		}

		struct DeviceRef { std::wstring id, name; };

		std::vector<DeviceRef> enumDevices() {
			std::vector<DeviceRef> out;
			auto mgr = create<IPortableDeviceManager>(CLSID_PortableDeviceManager);
			if (!mgr) return out;
			mgr->RefreshDeviceList();
			DWORD n = 0;
			if (FAILED(mgr->GetDevices(nullptr, &n)) || n == 0) return out;
			std::vector<PWSTR> ids(n);
			if (FAILED(mgr->GetDevices(ids.data(), &n))) return out;
			for (DWORD i = 0; i < n; ++i) {
				DeviceRef d;
				d.id = ids[i];
				DWORD len = 0;
				mgr->GetDeviceFriendlyName(ids[i], nullptr, &len);
				if (len) {
					std::wstring name(len, L'\0');
					if (SUCCEEDED(mgr->GetDeviceFriendlyName(ids[i], name.data(), &len))) d.name = name.c_str();
				}
				if (d.name.empty()) {
					len = 0;
					mgr->GetDeviceDescription(ids[i], nullptr, &len);
					if (len) {
						std::wstring desc(len, L'\0');
						if (SUCCEEDED(mgr->GetDeviceDescription(ids[i], desc.data(), &len))) d.name = desc.c_str();
					}
				}
				CoTaskMemFree(ids[i]);
				if (!d.name.empty()) out.push_back(std::move(d));
			}
			return out;
		}

		HRESULT openDevice(const std::wstring& id, ComPtr<IPortableDevice>& dev) {
			auto client = create<IPortableDeviceValues>(CLSID_PortableDeviceValues);
			if (!client) return E_FAIL;
			client->SetStringValue(WPD_CLIENT_NAME, L"foobar2000 Playlist Sync");
			client->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 1);
			client->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 0);
			client->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0);
			client->SetUnsignedIntegerValue(WPD_CLIENT_SECURITY_QUALITY_OF_SERVICE, SECURITY_IMPERSONATION);
			dev = create<IPortableDevice>(CLSID_PortableDeviceFTM);
			if (!dev) return E_FAIL;
			return dev->Open(id.c_str(), client.Get());
		}

		std::vector<std::wstring> childIds(IPortableDeviceContent* content, const std::wstring& parent) {
			std::vector<std::wstring> ids;
			ComPtr<IEnumPortableDeviceObjectIDs> e;
			if (FAILED(content->EnumObjects(0, parent.c_str(), nullptr, &e))) return ids;
			for (;;) {
				PWSTR batch[64] = {};
				DWORD got = 0;
				HRESULT hr = e->Next(64, batch, &got);
				for (DWORD i = 0; i < got; ++i) ids.push_back(takeString(batch[i]));
				if (hr != S_OK || got == 0) break;
			}
			return ids;
		}

		struct StorageRef { std::wstring id, name; };

		std::vector<StorageRef> enumStorages(IPortableDeviceContent* content, IPortableDeviceProperties* props) {
			std::vector<StorageRef> out;
			auto keys = create<IPortableDeviceKeyCollection>(CLSID_PortableDeviceKeyCollection);
			if (!keys) return out;
			keys->Add(WPD_OBJECT_NAME);
			keys->Add(WPD_FUNCTIONAL_OBJECT_CATEGORY);
			for (auto& id : childIds(content, WPD_DEVICE_OBJECT_ID)) {
				ComPtr<IPortableDeviceValues> v;
				if (FAILED(props->GetValues(id.c_str(), keys.Get(), &v))) continue;
				GUID cat{};
				if (FAILED(v->GetGuidValue(WPD_FUNCTIONAL_OBJECT_CATEGORY, &cat)) || cat != WPD_FUNCTIONAL_CATEGORY_STORAGE) continue;
				LPWSTR name = nullptr;
				v->GetStringValue(WPD_OBJECT_NAME, &name);
				out.push_back(StorageRef{ id, takeString(name) });
			}
			return out;
		}

		int64_t variantDateToFileTime(double date) {
			SYSTEMTIME st;
			FILETIME local, utc;
			if (!VariantTimeToSystemTime(date, &st) || !SystemTimeToFileTime(&st, &local) || !LocalFileTimeToFileTime(&local, &utc)) return 0;
			return (int64_t)(((uint64_t)utc.dwHighDateTime << 32) | utc.dwLowDateTime);
		}

		bool fileTimeToVariantDate(int64_t ft, double& out) {
			FILETIME utc, local;
			utc.dwLowDateTime = (DWORD)((uint64_t)ft & 0xFFFFFFFF);
			utc.dwHighDateTime = (DWORD)((uint64_t)ft >> 32);
			SYSTEMTIME st;
			return FileTimeToLocalFileTime(&utc, &local) && FileTimeToSystemTime(&local, &st) && SystemTimeToVariantTime(&st, &out);
		}

		using Reader = std::function<bool(BYTE* buf, DWORD want, DWORD& got)>;
		using ReaderFactory = std::function<Reader()>;

		class MtpFs : public DestFs {
		public:
			explicit MtpFs(std::wstring url) : m_url(std::move(url)) {}

			std::wstring describe() const override { return m_url; }

			bool openRoot(bool create, bool& rootMissing, std::wstring& err) override {
				rootMissing = false;
				std::wstring rest = m_url.substr(wcslen(kMtpPrefix));
				std::vector<std::wstring> parts = splitPath(rest);
				if (parts.size() < 2) {
					err = L"MTP destination must look like mtp://Device/Storage/Folder (got " + m_url + L")";
					return false;
				}
				// Device names may contain '/', so try progressively longer prefixes as the device name.
				auto devices = enumDevices();
				size_t used = 0;
				const DeviceRef* dev = nullptr;
				for (size_t n = 1; n < parts.size() && !dev; ++n) {
					std::wstring candidate = parts[0];
					for (size_t k = 1; k < n; ++k) candidate += L"/" + parts[k];
					for (auto& d : devices) if (sameName(d.name, candidate)) { dev = &d; used = n; break; }
				}
				if (!dev) {
					err = L"Device \"" + parts[0] + L"\" is not connected or not unlocked.";
					if (!devices.empty()) {
						err += L" Connected devices:";
						for (auto& d : devices) err += L" \"" + d.name + L"\"";
					}
					return false;
				}
				HRESULT hr = openDevice(dev->id, m_device);
				if (FAILED(hr)) { err = L"Couldn't open \"" + dev->name + L"\": " + hrText(hr) + L" (is the phone unlocked and set to File transfer?)"; return false; }
				if (FAILED(hr = m_device->Content(&m_content)) || FAILED(hr = m_content->Properties(&m_props))) {
					err = L"Couldn't access \"" + dev->name + L"\": " + hrText(hr);
					return false;
				}

				auto storages = enumStorages(m_content.Get(), m_props.Get());
				const StorageRef* storage = nullptr;
				for (auto& s : storages) if (sameName(s.name, parts[used])) { storage = &s; break; }
				if (!storage) {
					err = L"Storage \"" + parts[used] + L"\" not found on \"" + dev->name + L"\".";
					if (storages.empty()) err += L" The device shows no storage; unlock it and choose \"File transfer\".";
					else { err += L" Available:"; for (auto& s : storages) err += L" \"" + s.name + L"\""; }
					return false;
				}
				m_storageId = storage->id;

				// Walk (and create) the folder path below the storage.
				std::wstring id = m_storageId;
				for (size_t k = used + 1; k < parts.size(); ++k) {
					auto children = loadChildrenOf(id);
					const Child* found = nullptr;
					for (auto& kv : children) if (sameName(kv.second.name, parts[k])) { found = &kv.second; break; }
					if (found) {
						if (!found->isDir) { err = L"\"" + parts[k] + L"\" on the device is a file, not a folder."; return false; }
						id = found->id;
						continue;
					}
					if (!create) { rootMissing = true; return true; }
					if (!createFolder(id, parts[k], id, err)) return false;
				}
				m_dirs[L""] = Dir{ id };
				return true;
			}

			DestStat stat(const std::wstring& rel) override {
				DestStat st;
				const Child* c = findChild(rel);
				if (c) { st.exists = true; st.isDir = c->isDir; st.size = c->size; st.mtime = c->mtime; }
				return st;
			}

			void listFiles(const std::function<bool(const std::wstring&, uint64_t)>& cb) override {
				std::vector<std::wstring> stack{ L"" };
				while (!stack.empty()) {
					std::wstring relDir = stack.back();
					stack.pop_back();
					std::wstring dummy;
					Dir* d = getDir(relDir, false, dummy);
					if (!d) continue;
					for (auto& kv : d->children) {
						const Child& c = kv.second;
						std::wstring rel = relDir.empty() ? c.name : relDir + L"\\" + c.name;
						if (c.isDir) stack.push_back(rel);
						else if (!cb(rel, c.size)) return;
					}
				}
			}

			bool ensureDir(const std::wstring& relDir, std::wstring& err) override {
				return getDir(relDir, true, err) != nullptr;
			}

			bool copyIn(const std::wstring& src, uint64_t srcSize, const std::wstring& rel, bool replacing, const CopyTimes& times,
				CopyProgress& progress, bool& aborted, std::wstring& err) override {
				const std::wstring lp = src.rfind(L"\\\\", 0) == 0 ? (src.rfind(L"\\\\?\\", 0) == 0 ? src : L"\\\\?\\UNC\\" + src.substr(2)) : L"\\\\?\\" + src;
				std::shared_ptr<void> file(CreateFileW(lp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr),
					[](HANDLE h) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); });
				if (file.get() == INVALID_HANDLE_VALUE) { err = L"Couldn't open source: " + sysErrorText(GetLastError()); return false; }
				ReaderFactory factory = [file]() -> Reader {
					LARGE_INTEGER zero{};
					SetFilePointerEx(file.get(), zero, nullptr, FILE_BEGIN);
					return [file](BYTE* buf, DWORD want, DWORD& got) { return ReadFile(file.get(), buf, want, &got, nullptr) != 0; };
				};
				(void)replacing;
				return put(rel, srcSize, factory, times, &progress, aborted, err);
			}

			bool remove(const std::wstring& rel, std::wstring& err) override {
				std::wstring dummy;
				Dir* parent = getDir(parentRel(rel), false, dummy);
				const std::wstring leaf = leafOf(rel);
				auto it = parent ? parent->children.find(pathKey(leaf)) : decltype(parent->children)::iterator{};
				if (!parent || it == parent->children.end()) return true;
				HRESULT hr = deleteObject(it->second.id);
				if (FAILED(hr)) { err = L"Couldn't remove: " + hrText(hr); return false; }
				parent->children.erase(it);
				return true;
			}

			bool removeDirIfEmpty(const std::wstring& relDir) override {
				if (relDir.empty()) return false;
				std::wstring dummy;
				Dir* d = getDir(relDir, false, dummy);
				if (!d || !d->children.empty()) return false;
				if (FAILED(deleteObject(d->id))) return false;
				const std::wstring key = pathKey(relDir);
				m_dirs.erase(key);
				if (Dir* parent = getDir(parentRel(relDir), false, dummy)) parent->children.erase(pathKey(leafOf(relDir)));
				return true;
			}

			bool readFile(const std::wstring& rel, std::string& out) override {
				const Child* c = findChild(rel);
				if (!c || c->isDir || c->size > (64ull << 20)) return false;
				ComPtr<IPortableDeviceResources> res;
				if (FAILED(m_content->Transfer(&res))) return false;
				DWORD optimal = 0;
				ComPtr<IStream> stream;
				OpTimer timer(L"read");
				if (FAILED(res->GetStream(c->id.c_str(), WPD_RESOURCE_DEFAULT, STGM_READ, &optimal, &stream))) return false;
				out.clear();
				std::vector<char> buf(std::clamp<DWORD>(optimal, 64 * 1024, 1024 * 1024));
				for (;;) {
					ULONG got = 0;
					HRESULT hr = stream->Read(buf.data(), (ULONG)buf.size(), &got);
					if (FAILED(hr)) return false;
					if (got == 0) break;
					out.append(buf.data(), got);
					if (hr == S_FALSE) break;
				}
				return true;
			}

			bool writeFile(const std::wstring& rel, const std::string& data, bool, std::wstring& err) override {
				auto shared = std::make_shared<std::string>(data);
				ReaderFactory factory = [shared]() -> Reader {
					auto pos = std::make_shared<size_t>(0);
					return [shared, pos](BYTE* buf, DWORD want, DWORD& got) {
						got = (DWORD)std::min<size_t>(want, shared->size() - *pos);
						memcpy(buf, shared->data() + *pos, got);
						*pos += got;
						return true;
					};
				};
				bool aborted = false;
				return put(rel, data.size(), factory, CopyTimes{}, nullptr, aborted, err);
			}

			bool freeSpace(uint64_t& bytes) override {
				auto keys = create<IPortableDeviceKeyCollection>(CLSID_PortableDeviceKeyCollection);
				if (!keys) return false;
				keys->Add(WPD_STORAGE_FREE_SPACE_IN_BYTES);
				ComPtr<IPortableDeviceValues> v;
				ULONGLONG free = 0;
				if (FAILED(m_props->GetValues(m_storageId.c_str(), keys.Get(), &v)) ||
					FAILED(v->GetUnsignedLargeIntegerValue(WPD_STORAGE_FREE_SPACE_IN_BYTES, &free))) return false;
				bytes = free;
				return true;
			}

			bool hasReliableDates() const override { return false; }

		private:
			struct Child {
				std::wstring name, id;
				bool isDir = false;
				uint64_t size = 0;
				int64_t mtime = 0;
			};
			struct Dir {
				std::wstring id;
				bool loaded = false;
				std::unordered_map<std::wstring, Child> children; // keyed by pathKey(name)
			};

			static std::wstring leafOf(const std::wstring& rel) {
				auto pos = rel.find_last_of(L'\\');
				return pos == std::wstring::npos ? rel : rel.substr(pos + 1);
			}

			std::unordered_map<std::wstring, Child> loadChildrenOf(const std::wstring& parentId) {
				std::unordered_map<std::wstring, Child> out;
				auto keys = create<IPortableDeviceKeyCollection>(CLSID_PortableDeviceKeyCollection);
				if (!keys) return out;
				keys->Add(WPD_OBJECT_ORIGINAL_FILE_NAME);
				keys->Add(WPD_OBJECT_NAME);
				keys->Add(WPD_OBJECT_CONTENT_TYPE);
				keys->Add(WPD_OBJECT_SIZE);
				keys->Add(WPD_OBJECT_DATE_MODIFIED);
				OpTimer timer(L"list");
				for (auto& id : childIds(m_content.Get(), parentId)) {
					ComPtr<IPortableDeviceValues> v;
					if (FAILED(m_props->GetValues(id.c_str(), keys.Get(), &v))) continue;
					Child c;
					c.id = id;
					if (childFromValues(v.Get(), c)) out[pathKey(c.name)] = std::move(c);
				}
				return out;
			}

			static bool childFromValues(IPortableDeviceValues* v, Child& c) {
				LPWSTR s = nullptr;
				if (SUCCEEDED(v->GetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, &s)) && s && *s) c.name = takeString(s);
				else { CoTaskMemFree(s); s = nullptr; if (SUCCEEDED(v->GetStringValue(WPD_OBJECT_NAME, &s))) c.name = takeString(s); }
				if (c.name.empty()) return false;
				GUID type{};
				v->GetGuidValue(WPD_OBJECT_CONTENT_TYPE, &type);
				c.isDir = type == WPD_CONTENT_TYPE_FOLDER || type == WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT;
				ULONGLONG size = 0;
				if (SUCCEEDED(v->GetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, &size))) c.size = size;
				PROPVARIANT pv;
				PropVariantInit(&pv);
				if (SUCCEEDED(v->GetValue(WPD_OBJECT_DATE_MODIFIED, &pv)) && pv.vt == VT_DATE) c.mtime = variantDateToFileTime(pv.date);
				PropVariantClear(&pv);
				return true;
			}

			// Returns the cached folder for relDir, loading its listing; creates missing folders when create is set.
			Dir* getDir(const std::wstring& relDir, bool create, std::wstring& err) {
				const std::wstring key = pathKey(relDir);
				auto it = m_dirs.find(key);
				if (it == m_dirs.end()) {
					if (relDir.empty()) { err = L"Destination folder doesn't exist on the device."; return nullptr; }
					Dir* parent = getDir(parentRel(relDir), create, err);
					if (!parent) return nullptr;
					const std::wstring leaf = leafOf(relDir);
					auto c = parent->children.find(pathKey(leaf));
					std::wstring id;
					if (c != parent->children.end()) {
						if (!c->second.isDir) { err = L"A file named \"" + leaf + L"\" is in the way of a folder."; return nullptr; }
						id = c->second.id;
					} else {
						if (!create) return nullptr;
						if (!createFolder(parent->id, leaf, id, err)) return nullptr;
						Child nc; nc.name = leaf; nc.id = id; nc.isDir = true;
						parent->children[pathKey(leaf)] = nc;
						Dir fresh{ id };
						fresh.loaded = true;
						return &(m_dirs[key] = std::move(fresh));
					}
					it = m_dirs.emplace(key, Dir{ id }).first;
				}
				if (!it->second.loaded) {
					it->second.children = loadChildrenOf(it->second.id);
					it->second.loaded = true;
				}
				return &it->second;
			}

			const Child* findChild(const std::wstring& rel) {
				std::wstring dummy;
				Dir* d = getDir(parentRel(rel), false, dummy);
				if (!d) return nullptr;
				auto it = d->children.find(pathKey(leafOf(rel)));
				return it == d->children.end() ? nullptr : &it->second;
			}

			bool createFolder(const std::wstring& parentId, const std::wstring& name, std::wstring& newId, std::wstring& err) {
				auto v = create<IPortableDeviceValues>(CLSID_PortableDeviceValues);
				if (!v) { err = L"WPD unavailable"; return false; }
				v->SetStringValue(WPD_OBJECT_PARENT_ID, parentId.c_str());
				v->SetStringValue(WPD_OBJECT_NAME, name.c_str());
				v->SetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, name.c_str());
				v->SetGuidValue(WPD_OBJECT_CONTENT_TYPE, WPD_CONTENT_TYPE_FOLDER);
				v->SetGuidValue(WPD_OBJECT_FORMAT, WPD_OBJECT_FORMAT_PROPERTIES_ONLY);
				LPWSTR id = nullptr;
				OpTimer timer(L"mkdir");
				HRESULT hr = m_content->CreateObjectWithPropertiesOnly(v.Get(), &id);
				if (FAILED(hr)) { err = L"Couldn't create folder \"" + name + L"\": " + hrText(hr); return false; }
				newId = takeString(id);
				return true;
			}

			HRESULT deleteObject(const std::wstring& id) {
				auto coll = create<IPortableDevicePropVariantCollection>(CLSID_PortableDevicePropVariantCollection);
				if (!coll) return E_FAIL;
				PROPVARIANT pv;
				PropVariantInit(&pv);
				pv.vt = VT_LPWSTR;
				size_t bytes = (id.size() + 1) * sizeof(wchar_t);
				pv.pwszVal = (LPWSTR)CoTaskMemAlloc(bytes);
				if (!pv.pwszVal) return E_OUTOFMEMORY;
				memcpy(pv.pwszVal, id.c_str(), bytes);
				HRESULT hr = coll->Add(&pv);
				PropVariantClear(&pv);
				if (FAILED(hr)) return hr;
				OpTimer timer(L"delete");
				return m_content->Delete(PORTABLE_DEVICE_DELETE_NO_RECURSION, coll.Get(), nullptr);
			}

			bool renameObject(const std::wstring& id, const std::wstring& name) {
				auto v = create<IPortableDeviceValues>(CLSID_PortableDeviceValues);
				if (!v) return false;
				v->SetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, name.c_str());
				ComPtr<IPortableDeviceValues> results;
				OpTimer timer(L"rename");
				HRESULT hr = m_props->SetValues(id.c_str(), v.Get(), &results);
				if (hr != S_OK) return false;
				// Keep the display name in step; some devices don't allow it, which is harmless.
				v->Clear();
				v->SetStringValue(WPD_OBJECT_NAME, name.c_str());
				results.Reset();
				m_props->SetValues(id.c_str(), v.Get(), &results);
				return true;
			}

			bool upload(const std::wstring& parentId, const std::wstring& name, uint64_t size, const Reader& read,
				const CopyTimes& times, CopyProgress* progress, bool& aborted, std::wstring& newId, std::wstring& err) {
				aborted = false;
				auto v = create<IPortableDeviceValues>(CLSID_PortableDeviceValues);
				if (!v) { err = L"WPD unavailable"; return false; }
				v->SetStringValue(WPD_OBJECT_PARENT_ID, parentId.c_str());
				v->SetStringValue(WPD_OBJECT_NAME, name.c_str());
				v->SetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, name.c_str());
				v->SetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, size);
				v->SetGuidValue(WPD_OBJECT_CONTENT_TYPE, WPD_CONTENT_TYPE_GENERIC_FILE);
				v->SetGuidValue(WPD_OBJECT_FORMAT, WPD_OBJECT_FORMAT_UNSPECIFIED);
				if (times.set) {
					// Many phones ignore these and use the upload time; sync decisions don't depend on them.
					PROPVARIANT pv;
					PropVariantInit(&pv);
					pv.vt = VT_DATE;
					if (fileTimeToVariantDate(times.mtime, pv.date)) v->SetValue(WPD_OBJECT_DATE_MODIFIED, &pv);
					if (fileTimeToVariantDate(times.ctime, pv.date)) v->SetValue(WPD_OBJECT_DATE_CREATED, &pv);
				}

				ComPtr<IStream> stream;
				DWORD optimal = 0;
				OpTimer timer(L"upload");
				HRESULT hr = m_content->CreateObjectWithPropertiesAndData(v.Get(), &stream, &optimal, nullptr);
				if (FAILED(hr)) { err = L"Couldn't start upload: " + hrText(hr); return false; }

				std::vector<BYTE> buf(std::clamp<DWORD>(optimal, 64 * 1024, 1024 * 1024));
				uint64_t done = 0;
				while (done < size) {
					if (progress && progress->abortRequested()) { stream->Revert(); aborted = true; return false; }
					DWORD want = (DWORD)std::min<uint64_t>(buf.size(), size - done);
					DWORD got = 0;
					if (!read(buf.data(), want, got) || got == 0) { stream->Revert(); err = L"Couldn't read the source file"; return false; }
					ULONG written = 0;
					hr = stream->Write(buf.data(), got, &written);
					if (FAILED(hr) || written != got) { stream->Revert(); err = L"Upload failed: " + hrText(FAILED(hr) ? hr : E_FAIL); return false; }
					done += got;
					if (progress && size) progress->setFileProgress(double(done) / size);
				}
				hr = stream->Commit(STGC_DEFAULT);
				if (FAILED(hr)) { err = L"Upload failed: " + hrText(hr); return false; }
				ComPtr<IPortableDeviceDataStream> ds;
				LPWSTR id = nullptr;
				if (SUCCEEDED(stream.As(&ds)) && SUCCEEDED(ds->GetObjectID(&id))) newId = takeString(id);
				return true;
			}

			// Uploads rel; an existing file is replaced via upload-as-temp, delete, rename so it's never lost
			// to an interrupted transfer. Devices that refuse renames get delete + direct upload.
			bool put(const std::wstring& rel, uint64_t size, const ReaderFactory& factory, const CopyTimes& times,
				CopyProgress* progress, bool& aborted, std::wstring& err) {
				Dir* parent = getDir(parentRel(rel), true, err);
				if (!parent) return false;
				const std::wstring name = leafOf(rel);
				const std::wstring key = pathKey(name);
				auto existing = parent->children.find(key);
				std::wstring newId;

				auto record = [&](const std::wstring& id) {
					Child c; c.name = name; c.id = id; c.size = size;
					FILETIME now; GetSystemTimeAsFileTime(&now);
					c.mtime = (int64_t)(((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime);
					parent->children[key] = c;
				};

				if (existing == parent->children.end()) {
					if (!upload(parent->id, name, size, factory(), times, progress, aborted, newId, err)) return false;
					record(newId);
					return true;
				}

				const std::wstring oldId = existing->second.id;
				const std::wstring tmpName = name + kTempSuffix;
				auto staleTmp = parent->children.find(pathKey(tmpName));
				if (staleTmp != parent->children.end()) {
					deleteObject(staleTmp->second.id);
					parent->children.erase(staleTmp);
				}
				std::wstring tmpId;
				if (!upload(parent->id, tmpName, size, factory(), times, progress, aborted, tmpId, err)) return false;
				HRESULT hr = deleteObject(oldId);
				if (FAILED(hr)) {
					deleteObject(tmpId);
					err = L"Couldn't replace the file on the device: " + hrText(hr);
					return false;
				}
				parent->children.erase(key);
				if (!tmpId.empty() && renameObject(tmpId, name)) {
					record(tmpId);
					return true;
				}
				// No rename support: the old copy is already gone, upload under the final name.
				if (!tmpId.empty()) deleteObject(tmpId);
				if (!upload(parent->id, name, size, factory(), times, progress, aborted, newId, err)) return false;
				record(newId);
				return true;
			}

			std::wstring m_url;
			ComPtr<IPortableDevice> m_device;
			ComPtr<IPortableDeviceContent> m_content;
			ComPtr<IPortableDeviceProperties> m_props;
			std::wstring m_storageId;
			std::unordered_map<std::wstring, Dir> m_dirs; // keyed by pathKey(relative folder), "" = root
		};
	}

	std::unique_ptr<DestFs> createMtpFs(const std::wstring& url) { return std::make_unique<MtpFs>(url); }

	std::vector<MtpStorageInfo> listMtpStorages(bool includeMassStorage) {
		std::vector<MtpStorageInfo> out;
		for (auto& d : enumDevices()) {
			ComPtr<IPortableDevice> dev;
			ComPtr<IPortableDeviceContent> content;
			ComPtr<IPortableDeviceProperties> props;
			if (FAILED(openDevice(d.id, dev)) || FAILED(dev->Content(&content)) || FAILED(content->Properties(&props))) continue;
			if (!includeMassStorage) {
				// WPD also exposes ordinary drives ("MSC: ..."); those already have drive letters.
				auto keys = create<IPortableDeviceKeyCollection>(CLSID_PortableDeviceKeyCollection);
				ComPtr<IPortableDeviceValues> v;
				LPWSTR protocol = nullptr;
				if (keys && SUCCEEDED(keys->Add(WPD_DEVICE_PROTOCOL)) && SUCCEEDED(props->GetValues(WPD_DEVICE_OBJECT_ID, keys.Get(), &v)) &&
					SUCCEEDED(v->GetStringValue(WPD_DEVICE_PROTOCOL, &protocol))) {
					const std::wstring p = takeString(protocol);
					if (_wcsnicmp(p.c_str(), L"MSC", 3) == 0) { dev->Close(); continue; }
				}
			}
			for (auto& s : enumStorages(content.Get(), props.Get())) out.push_back(MtpStorageInfo{ d.name, s.name });
			dev->Close();
		}
		return out;
	}
}
