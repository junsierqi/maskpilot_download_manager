"""Owned per-operation HTTP adapter. Source credentials arrive only over a local capability."""
import http.client
import http.cookiejar
import ipaddress
import json
import os
import urllib.error
import urllib.request
from urllib.parse import urlsplit

from yt_dlp.extractor.common import InfoExtractor
from yt_dlp.networking._urllib import UrllibRH
from yt_dlp.networking.common import _REQUEST_HANDLERS


def _bootstrap():
    endpoint = urlsplit(os.environ.pop("MASKPILOT_MEDIA_SCOPE_ENDPOINT", ""))
    if (endpoint.scheme != "http" or endpoint.hostname != "127.0.0.1" or not endpoint.port
            or endpoint.username or endpoint.password or endpoint.query or endpoint.fragment
            or not endpoint.path.startswith("/scope/")):
        raise RuntimeError("Invalid owned media routing capability")
    return endpoint


_endpoint = _bootstrap()


def _control(suffix):
    connection = http.client.HTTPConnection("127.0.0.1", _endpoint.port, timeout=5)
    try:
        connection.request("GET", _endpoint.path + suffix, headers={"Connection": "close"})
        response = connection.getresponse()
        data = response.read(256 * 1024 + 1)
        if response.status != 200 or len(data) > 256 * 1024:
            raise RuntimeError("Owned media routing is unavailable")
        return data
    finally:
        connection.close()


_policy = json.loads(_control(""))
if (_policy.get("version") != 1 or not isinstance(_policy.get("origins"), dict)
        or not 1 <= len(_policy["origins"]) <= 17):
    raise RuntimeError("Unsupported owned media routing policy")


def _origin(url):
    parsed = urlsplit(url)
    if (parsed.scheme not in ("http", "https") or not parsed.hostname
            or parsed.username is not None or parsed.password is not None or parsed.fragment):
        raise urllib.error.URLError("Media origin is not approved")
    host = parsed.hostname.lower()
    try:
        address = ipaddress.ip_address(host)
        host = "[" + address.exploded + "]" if address.version == 6 else str(address)
    except ValueError:
        pass
    return f'{parsed.scheme}://{host}:{parsed.port or (443 if parsed.scheme == "https" else 80)}'


class _ScopedHeaders(urllib.request.BaseHandler):
    # After default HTTP and cookie processors, including on redirects.
    handler_order = 1000

    def http_request(self, request):
        origin = _origin(request.full_url)
        if origin not in _policy["origins"] or request.get_method() not in ("GET", "HEAD"):
            raise urllib.error.URLError("Media origin or request method is not approved")
        safe = {"accept", "accept-encoding", "range", "if-range", "if-none-match",
                "if-modified-since", "user-agent"}
        retained = {name: value for name, value in request.header_items() if name.lower() in safe}
        request.headers.clear()
        request.unredirected_hdrs.clear()
        for name, value in retained.items():
            request.add_header(name, value)
        request.add_unredirected_header("Host", urlsplit(request.full_url).netloc)
        for name, value in _policy["origins"][origin].items():
            request.add_unredirected_header(name, value)
        return request

    https_request = http_request
    # Redirects are handled inside the opener, after the outer backend scheme check.
    # Install rejecting processors for its built-in non-HTTP handlers as well.
    ftp_request = file_request = data_request = http_request


# One known backend prevents an alternate handler from bypassing the final processor.
if not callable(getattr(UrllibRH, "_create_instance", None)) or UrllibRH.RH_KEY not in _REQUEST_HANDLERS:
    raise RuntimeError("Media component does not support owned request routing")
_original_create = UrllibRH._create_instance


def _create(self, *args, **kwargs):
    if not self.verify:
        raise RuntimeError("Verified TLS is required for owned media routing")
    opener = _original_create(self, *args, **kwargs)
    if not isinstance(opener, urllib.request.OpenerDirector):
        raise RuntimeError("Unsupported media request opener")
    opener.add_handler(_ScopedHeaders())
    return opener


UrllibRH._create_instance = _create
UrllibRH._SUPPORTED_URL_SCHEMES = ("http", "https")
UrllibRH._get_proxies = lambda self, request: {"http": _policy["proxy"], "https": _policy["proxy"]}
# The standard proxy opener otherwise consults system localhost/PAC bypass rules.
# This operation has one mandatory native route, including for loopback test origins.
urllib.request.proxy_bypass = lambda host: False
_REQUEST_HANDLERS.clear()
_REQUEST_HANDLERS[UrllibRH.RH_KEY] = UrllibRH
# Prove the actual opener factory and final processors before acknowledging access.
_probe = UrllibRH(logger=None)
_opener = _probe._create_instance(proxies={"http": _policy["proxy"], "https": _policy["proxy"]},
                                  cookiejar=http.cookiejar.CookieJar())
for _scheme in ("http", "https", "ftp", "file", "data"):
    _processors = _opener.process_request.get(_scheme, [])
    if not _processors or not isinstance(_processors[-1], _ScopedHeaders):
        raise RuntimeError("Media request processors are incompatible")
del _opener, _probe, _processors, _scheme
_control("/ready")


class MaskPilotScopedRequestsIE(InfoExtractor):
    _VALID_URL = r"(?!)"

    def _real_extract(self, url):
        raise RuntimeError("Internal request adapter cannot extract a source")
