// Attaches the dashboard's CSRF token to every same-origin state-changing
// request the page makes. page_layout_wrap() (page_layout.c) only loads this
// file - next to <meta name="csrf-token"> - on epoch-3 pages served to a
// signed-in user; the server checks the token in require_dashboard_session()
// (http_router.c) for every dashboard POST.
//
//   fetch() / XMLHttpRequest -> "X-CSRF-Token" request header
//   <form method="post">     -> hidden "csrf_token" field, added on submit
(function () {
  'use strict';

  var meta = document.querySelector('meta[name="csrf-token"]');
  var token = meta && meta.getAttribute('content');
  if (!token) return;

  var HEADER = 'X-CSRF-Token';
  var FIELD = 'csrf_token';

  function isSameOrigin(url) {
    try { return new URL(url, location.href).origin === location.origin; }
    catch (e) { return false; }
  }

  function isUnsafe(method) {
    return !/^(GET|HEAD|OPTIONS)$/i.test(method || 'GET');
  }

  var nativeFetch = window.fetch;
  if (nativeFetch) {
    window.fetch = function (input, init) {
      var isRequest = typeof Request !== 'undefined' && input instanceof Request;
      var url = isRequest ? input.url : String(input);
      var method = (init && init.method) || (isRequest ? input.method : 'GET');
      if (isUnsafe(method) && isSameOrigin(url)) {
        init = Object.assign({}, init);
        var headers = new Headers(init.headers || (isRequest ? input.headers : undefined));
        headers.set(HEADER, token);
        init.headers = headers;
      }
      return nativeFetch.call(this, input, init);
    };
  }

  var nativeOpen = XMLHttpRequest.prototype.open;
  var nativeSend = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.open = function (method, url) {
    this._boatRudderCsrf = isUnsafe(method) && isSameOrigin(url);
    return nativeOpen.apply(this, arguments);
  };
  XMLHttpRequest.prototype.send = function () {
    if (this._boatRudderCsrf) this.setRequestHeader(HEADER, token);
    return nativeSend.apply(this, arguments);
  };

  // Capture phase: runs before any page handler that might serialize the
  // form itself (new FormData(form)).
  document.addEventListener('submit', function (event) {
    var form = event.target;
    if (!(form instanceof HTMLFormElement)) return;
    if (!isUnsafe(form.method) || !isSameOrigin(form.action)) return;

    var input = form.querySelector('input[name="' + FIELD + '"]');
    if (!input) {
      input = document.createElement('input');
      input.type = 'hidden';
      input.name = FIELD;
      form.appendChild(input);
    }
    input.value = token;
  }, true);
})();
