// Both manifests share behavior; Firefox exposes the promise-based browser namespace.
const globals = globalThis as typeof globalThis & {browser?: typeof chrome};
export const browserApi = globals.browser ?? chrome;
const manifest = browserApi.runtime.getManifest() as chrome.runtime.Manifest & {
  browser_specific_settings?: {gecko?: {id?: string}};
};
export const isFirefox = manifest.browser_specific_settings?.gecko?.id === 'downloads@maskpilot.invalid';
