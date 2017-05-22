// MIT License

// Copyright (c) 2016 Mohammed Boujemaoui Boulaghmoudi

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "atom/browser/ui/win/toast_lib.h"

#include <string>

#include "brightray/browser/win/scoped_hstring.h"

#pragma comment(lib, "shlwapi")
#pragma comment(lib, "user32")

using WinToastLib::WinToast;
using WinToastLib::WinToastHandler;
using WinToastLib::WinToastStringWrapper;
using WinToastLib::WinToastTemplate;

namespace DllImporter {

// Function load a function from library
template <typename Function>
HRESULT loadFunctionFromLibrary(HINSTANCE library,
                                LPCSTR name,
                                Function& func) { // NOLINT
  if (!library)
    return false;

  func = reinterpret_cast<Function>(GetProcAddress(library, name));
  return (func != nullptr) ? S_OK : E_FAIL;
}

typedef HRESULT(FAR STDAPICALLTYPE* f_SetCurrentProcessExplicitAppUserModelID)(
    __in PCWSTR AppID);
typedef HRESULT(FAR STDAPICALLTYPE* f_PropVariantToString)(
    _In_ REFPROPVARIANT propvar,
    _Out_writes_(cch) PWSTR psz,
    _In_ UINT cch);
typedef HRESULT(FAR STDAPICALLTYPE* f_RoGetActivationFactory)(
    _In_ HSTRING activatableClassId,
    _In_ REFIID iid,
    _COM_Outptr_ void** factory);
typedef HRESULT(FAR STDAPICALLTYPE* f_WindowsCreateStringReference)(
    _In_reads_opt_(length + 1) PCWSTR sourceString,
    UINT32 length,
    _Out_ HSTRING_HEADER* hstringHeader,
    _Outptr_result_maybenull_ _Result_nullonfailure_ HSTRING* string);
typedef HRESULT(FAR STDAPICALLTYPE* f_WindowsDeleteString)(
    _In_opt_ HSTRING string);

f_SetCurrentProcessExplicitAppUserModelID
    SetCurrentProcessExplicitAppUserModelID;
f_PropVariantToString PropVariantToString;
f_RoGetActivationFactory RoGetActivationFactory;
f_WindowsCreateStringReference WindowsCreateStringReference;
f_WindowsDeleteString WindowsDeleteString;

template <class T>
_Check_return_ __inline HRESULT _1_GetActivationFactory(
    _In_ HSTRING activatableClassId,
    _COM_Outptr_ T** factory) {
  return RoGetActivationFactory(activatableClassId, IID_INS_ARGS(factory));
}

template <typename T>
inline HRESULT Wrap_GetActivationFactory(
    _In_ HSTRING activatableClassId,
    _Inout_ ComPtrRef<T> factory) throw() {
  return _1_GetActivationFactory(activatableClassId,
                                 factory.ReleaseAndGetAddressOf());
}

inline HRESULT initialize() {
  HINSTANCE LibShell32 = LoadLibrary(L"SHELL32.DLL");
  HRESULT hr = loadFunctionFromLibrary(
      LibShell32, "SetCurrentProcessExplicitAppUserModelID",
      SetCurrentProcessExplicitAppUserModelID);
  if (SUCCEEDED(hr)) {
    HINSTANCE LibPropSys = LoadLibrary(L"PROPSYS.DLL");
    hr = loadFunctionFromLibrary(LibPropSys, "PropVariantToString",
                                 PropVariantToString);
    if (SUCCEEDED(hr)) {
      HINSTANCE LibComBase = LoadLibrary(L"COMBASE.DLL");
      return SUCCEEDED(loadFunctionFromLibrary(LibComBase,
                                               "RoGetActivationFactory",
                                               RoGetActivationFactory)) &&
             SUCCEEDED(loadFunctionFromLibrary(LibComBase,
                                               "WindowsCreateStringReference",
                                               WindowsCreateStringReference)) &&
             SUCCEEDED(loadFunctionFromLibrary(
                 LibComBase, "WindowsDeleteString", WindowsDeleteString));
    }
  }
  return hr;
}

}  // namespace DllImporter

namespace Util {
inline HRESULT defaultExecutablePath(_In_ WCHAR* path,
                                     _In_ DWORD nSize = MAX_PATH) {
  DWORD written =
      GetModuleFileNameEx(GetCurrentProcess(), nullptr, path, nSize);
  return (written > 0) ? S_OK : E_FAIL;
}

inline HRESULT defaultShellLinksDirectory(_In_ WCHAR* path,
                                          _In_ DWORD nSize = MAX_PATH) {
  DWORD written = GetEnvironmentVariable(L"APPDATA", path, nSize);
  HRESULT hr = written > 0 ? S_OK : E_INVALIDARG;
  if (SUCCEEDED(hr)) {
    errno_t result = wcscat_s(path, nSize, DEFAULT_SHELL_LINKS_PATH);
    hr = (result == 0) ? S_OK : E_INVALIDARG;
  }
  return hr;
}

inline HRESULT defaultShellLinkPath(const std::wstring& appname,
                                    _In_ WCHAR* path,
                                    _In_ DWORD nSize = MAX_PATH) {
  HRESULT hr = defaultShellLinksDirectory(path, nSize);
  if (SUCCEEDED(hr)) {
    const std::wstring appLink(appname + DEFAULT_LINK_FORMAT);
    errno_t result = wcscat_s(path, nSize, appLink.c_str());
    hr = (result == 0) ? S_OK : E_INVALIDARG;
  }
  return hr;
}

inline HRESULT setNodeStringValue(const std::wstring& string,
                                  IXmlNode* node,
                                  IXmlDocument* xml) {
  ComPtr<IXmlText> textNode;
  HRESULT hr =
      xml->CreateTextNode(WinToastStringWrapper(string).Get(), &textNode);
  if (SUCCEEDED(hr)) {
    ComPtr<IXmlNode> stringNode;
    hr = textNode.As(&stringNode);
    if (SUCCEEDED(hr)) {
      ComPtr<IXmlNode> appendedChild;
      hr = node->AppendChild(stringNode.Get(), &appendedChild);
    }
  }
  return hr;
}

inline HRESULT setEventHandlers(_In_ IToastNotification* notification,
                                _In_ WinToastHandler* eventHandler) {
  EventRegistrationToken activatedToken, dismissedToken, failedToken;
  HRESULT hr = notification->add_Activated(
      Callback<
          Implements<RuntimeClassFlags<ClassicCom>,
                     ITypedEventHandler<ToastNotification*, IInspectable*>>>(
          [eventHandler](IToastNotification*, IInspectable*) {
            eventHandler->toastActivated();
            return S_OK;
          })
          .Get(),
      &activatedToken);

  if (SUCCEEDED(hr)) {
    hr = notification->add_Dismissed(
        Callback<Implements<
            RuntimeClassFlags<ClassicCom>,
            ITypedEventHandler<ToastNotification*, ToastDismissedEventArgs*>>>(
            [eventHandler](IToastNotification*, IToastDismissedEventArgs* e) {
              ToastDismissalReason reason;
              if (SUCCEEDED(e->get_Reason(&reason))) {
                eventHandler->toastDismissed(
                    static_cast<WinToastHandler::WinToastDismissalReason>(
                        reason));
              }
              return S_OK;
            })
            .Get(),
        &dismissedToken);
    if (SUCCEEDED(hr)) {
      hr = notification->add_Failed(
          Callback<Implements<
              RuntimeClassFlags<ClassicCom>,
              ITypedEventHandler<ToastNotification*, ToastFailedEventArgs*>>>(
              [eventHandler](IToastNotification*, IToastFailedEventArgs*) {
                eventHandler->toastFailed();
                return S_OK;
              })
              .Get(),
          &failedToken);
    }
  }
  return hr;
}

}  // namespace Util

WinToast* WinToast::_instance = nullptr;
WinToast* WinToast::instance() {
  if (_instance == nullptr) {
    _instance = new WinToast();
  }
  return _instance;
}

WinToast::WinToast() : _isInitialized(false) {
  DllImporter::initialize();
}

void WinToast::setAppName(_In_ const std::wstring& appName) {
  _appName = appName;
}

std::wstring WinToast::appName() const {
  return _appName;
}

std::wstring WinToast::appUserModelId() const {
  return _aumi;
}
void WinToast::setAppUserModelId(_In_ const std::wstring& aumi) {
  _aumi = aumi;
}

bool WinToast::isCompatible() {
  return !((DllImporter::SetCurrentProcessExplicitAppUserModelID == nullptr) ||
           (DllImporter::PropVariantToString == nullptr) ||
           (DllImporter::RoGetActivationFactory == nullptr) ||
           (DllImporter::WindowsCreateStringReference == nullptr) ||
           (DllImporter::WindowsDeleteString == nullptr));
}

bool WinToast::initialize() {
  if (_aumi.empty() || _appName.empty()) {
    return _isInitialized = false;
  }

  if (!isCompatible()) {
    return _isInitialized = false;
  }

  if (FAILED(DllImporter::SetCurrentProcessExplicitAppUserModelID(
          _aumi.c_str()))) {
    return _isInitialized = false;
  }

  HRESULT hr = DllImporter::Wrap_GetActivationFactory(
      WinToastStringWrapper(
          RuntimeClass_Windows_UI_Notifications_ToastNotificationManager)
          .Get(),
      &_notificationManager);
  if (SUCCEEDED(hr)) {
    hr = notificationManager()->CreateToastNotifierWithId(
        WinToastStringWrapper(_aumi).Get(), &_notifier);
    if (SUCCEEDED(hr)) {
      hr = DllImporter::Wrap_GetActivationFactory(
          WinToastStringWrapper(
              RuntimeClass_Windows_UI_Notifications_ToastNotification)
              .Get(),
          &_notificationFactory);
    }
  }

  return _isInitialized = SUCCEEDED(hr);
}

bool WinToast::showToast(_In_ const WinToastTemplate& toast,
                         _In_ WinToastHandler* handler) {
  if (!isInitialized()) {
    return _isInitialized;
  }

  HRESULT hr = _notificationManager->GetTemplateContent(
      ToastTemplateType(toast.type()), &_xmlDocument);
  if (SUCCEEDED(hr)) {
    const int fieldsCount = toast.textFieldsCount();
    for (int i = 0; i < fieldsCount && SUCCEEDED(hr); i++) {
      hr = setTextField(toast.textField(WinToastTemplate::TextField(i)), i);
    }
    if (makeSilent(toast.isSilent())) {
      if (SUCCEEDED(hr)) {
        if (SUCCEEDED(hr)) {
          hr = toast.hasImage() ? setImageField(toast.imagePath()) : hr;
          if (SUCCEEDED(hr)) {
            hr = _notificationFactory->CreateToastNotification(xmlDocument(),
                                                               &_notification);
            if (SUCCEEDED(hr)) {
              hr = Util::setEventHandlers(notification(), handler);
              if (SUCCEEDED(hr)) {
                hr = _notifier->Show(notification());
              }
            }
          }
        }
      }
    } else {
      return false;
    }
  }

  return SUCCEEDED(hr);
}

HRESULT WinToast::setTextField(_In_ const std::wstring& text, _In_ int pos) {
  ComPtr<IXmlNodeList> nodeList;
  HRESULT hr = _xmlDocument->GetElementsByTagName(
      WinToastStringWrapper(L"text").Get(), &nodeList);
  if (SUCCEEDED(hr)) {
    ComPtr<IXmlNode> node;
    hr = nodeList->Item(pos, &node);
    if (SUCCEEDED(hr)) {
      hr = Util::setNodeStringValue(text, node.Get(), xmlDocument());
    }
  }
  return hr;
}

bool WinToast::makeSilent(bool is_silent) {
  if (!is_silent)
    return true;
  ScopedHString tag(L"toast");
  if (!tag.success())
    return false;

  ComPtr<IXmlNodeList> node_list;
  if (FAILED(xmlDocument()->GetElementsByTagName(tag, &node_list)))
    return false;

  ComPtr<IXmlNode> root;
  if (FAILED(node_list->Item(0, &root)))
    return false;

  ComPtr<IXmlElement> audio_element;
  ScopedHString audio_str(L"audio");
  if (FAILED(xmlDocument()->CreateElement(audio_str, &audio_element)))
    return false;

  ComPtr<IXmlNode> audio_node_tmp;
  if (FAILED(audio_element.As(&audio_node_tmp)))
    return false;

  // Append audio node to toast xml
  ComPtr<IXmlNode> audio_node;
  if (FAILED(root->AppendChild(audio_node_tmp.Get(), &audio_node)))
    return false;

  // Create silent attribute
  ComPtr<IXmlNamedNodeMap> attributes;
  if (FAILED(audio_node->get_Attributes(&attributes)))
    return false;

  ComPtr<IXmlAttribute> silent_attribute;
  ScopedHString silent_str(L"silent");
  if (FAILED(xmlDocument()->CreateAttribute(silent_str, &silent_attribute)))
    return false;

  ComPtr<IXmlNode> silent_attribute_node;
  if (FAILED(silent_attribute.As(&silent_attribute_node)))
    return false;

  // Set silent attribute to true
  ScopedHString silent_value(L"true");
  if (!silent_value.success())
    return false;

  ComPtr<IXmlText> silent_text;
  if (FAILED(xmlDocument()->CreateTextNode(silent_value, &silent_text)))
    return false;

  ComPtr<IXmlNode> silent_node;
  if (FAILED(silent_text.As(&silent_node)))
    return false;

  ComPtr<IXmlNode> child_node;
  if (FAILED(
          silent_attribute_node->AppendChild(silent_node.Get(), &child_node)))
    return false;

  ComPtr<IXmlNode> silent_attribute_pnode;
  return SUCCEEDED(attributes.Get()->SetNamedItem(silent_attribute_node.Get(),
                                                  &silent_attribute_pnode));
}

HRESULT WinToast::setImageField(_In_ const std::wstring& path) {
  wchar_t imagePath[MAX_PATH] = L"file:///";
  HRESULT hr = StringCchCat(imagePath, MAX_PATH, path.c_str());
  if (SUCCEEDED(hr)) {
    ComPtr<IXmlNodeList> nodeList;
    hr = _xmlDocument->GetElementsByTagName(
        WinToastStringWrapper(L"image").Get(), &nodeList);
    if (SUCCEEDED(hr)) {
      ComPtr<IXmlNode> node;
      hr = nodeList->Item(0, &node);
      if (SUCCEEDED(hr)) {
        ComPtr<IXmlNamedNodeMap> attributes;
        hr = node->get_Attributes(&attributes);
        if (SUCCEEDED(hr)) {
          ComPtr<IXmlNode> editedNode;
          hr = attributes->GetNamedItem(WinToastStringWrapper(L"src").Get(),
                                        &editedNode);
          if (SUCCEEDED(hr)) {
            Util::setNodeStringValue(imagePath, editedNode.Get(),
                                     xmlDocument());
          }
        }
      }
    }
  }
  return hr;
}

WinToastTemplate::WinToastTemplate(const WinToastTemplateType& type)
    : _type(type) {
  initComponentsFromType();
}

WinToastTemplate::~WinToastTemplate() {
  _textFields.clear();
}

void WinToastTemplate::setTextField(
    _In_ const std::wstring& txt,
    _In_ const WinToastTemplate::TextField& pos) {
  _textFields[pos] = txt;
}
void WinToastTemplate::setImagePath(_In_ const std::wstring& imgPath) {
  if (!_hasImage)
    return;
  _imagePath = imgPath;
}

void WinToastTemplate::setSilent(bool is_silent) {
  _silent = is_silent;
}

int WinToastTemplate::TextFieldsCount[WinToastTemplateTypeCount] = {1, 2, 2, 3,
                                                                    1, 2, 2, 3};
void WinToastTemplate::initComponentsFromType() {
  _hasImage = _type < ToastTemplateType_ToastText01;
  _textFields = std::vector<std::wstring>(TextFieldsCount[_type], L"");
}

WinToastStringWrapper::WinToastStringWrapper(_In_reads_(length)
                                                 PCWSTR stringRef,
                                             _In_ UINT32 length) throw() {
  HRESULT hr = DllImporter::WindowsCreateStringReference(stringRef, length,
                                                         &_header, &_hstring);
  if (!SUCCEEDED(hr)) {
    RaiseException(static_cast<DWORD>(STATUS_INVALID_PARAMETER),
                   EXCEPTION_NONCONTINUABLE, 0, nullptr);
  }
}

WinToastStringWrapper::WinToastStringWrapper(const std::wstring& stringRef) {
  HRESULT hr = DllImporter::WindowsCreateStringReference(
      stringRef.c_str(), static_cast<UINT32>(stringRef.length()), &_header,
      &_hstring);
  if (FAILED(hr)) {
    RaiseException(static_cast<DWORD>(STATUS_INVALID_PARAMETER),
                   EXCEPTION_NONCONTINUABLE, 0, nullptr);
  }
}

WinToastStringWrapper::~WinToastStringWrapper() {
  DllImporter::WindowsDeleteString(_hstring);
}

HSTRING WinToastStringWrapper::Get() const {
  return _hstring;
}
