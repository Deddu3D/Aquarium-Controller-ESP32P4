/* Aquarium Controller – Service Worker (PWA)
 * Provides offline fallback and basic asset caching.
 */

const CACHE_NAME = 'aquarium-v1';
const STATIC_ASSETS = [
  '/',
  '/www/style.css',
  '/www/bg.svg',
  '/manifest.json',
];

self.addEventListener('install', event => {
  event.waitUntil(
    caches.open(CACHE_NAME).then(cache => cache.addAll(STATIC_ASSETS))
  );
  self.skipWaiting();
});

self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys.filter(k => k !== CACHE_NAME).map(k => caches.delete(k)))
    )
  );
  self.clients.claim();
});

self.addEventListener('fetch', event => {
  const url = new URL(event.request.url);

  /* Always fetch API calls and WebSocket from the network */
  if (url.pathname.startsWith('/api/') || url.pathname === '/ws') {
    return;
  }

  event.respondWith(
    caches.match(event.request).then(cached => {
      if (cached) return cached;
      return fetch(event.request).then(response => {
        /* Cache successful GET responses for static assets */
        if (event.request.method === 'GET' && response.status === 200) {
          const cloned = response.clone();
          caches.open(CACHE_NAME).then(cache => cache.put(event.request, cloned));
        }
        return response;
      }).catch(() => {
        /* Offline fallback for navigation requests */
        if (event.request.mode === 'navigate') {
          return caches.match('/');
        }
      });
    })
  );
});
