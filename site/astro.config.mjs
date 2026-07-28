// @ts-check
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import starlightLinksValidator from 'starlight-links-validator';

export default defineConfig({
  site: 'https://rar.github.io',
  base: '/esphome-tigomonitor',
  integrations: [
    starlight({
      title: 'Tigo Monitor',
      // "Emitting" brand mark. Three cuts of the same drawing, each tuned to
      // its render size — see the comments in each file. The favicon matches
      // the one the device itself serves, so a docs tab and a live-rig tab
      // carry the same icon.
      logo: { src: './src/assets/logo.svg' },
      favicon: '/favicon.svg',
      customCss: [
        // Self-hosted fonts (bundled, no CDN). Order matters: fonts first.
        '@fontsource/ibm-plex-sans/400.css',
        '@fontsource/ibm-plex-sans/500.css',
        '@fontsource/ibm-plex-sans/600.css',
        '@fontsource/ibm-plex-sans/700.css',
        '@fontsource/ibm-plex-mono/400.css',
        '@fontsource/ibm-plex-mono/500.css',
        '@fontsource/ibm-plex-mono/600.css',
        '@fontsource-variable/big-shoulders-display',
        './src/styles/theme.css',
      ],
      // Build-time gate: fails the build on broken internal links + anchors,
      // so relative-link/base-path regressions (see the 2026-07-23 review) can't
      // silently ship again.
      plugins: [starlightLinksValidator()],
      social: [
        { icon: 'github', label: 'GitHub', href: 'https://github.com/RAR/esphome-tigomonitor' },
      ],
      // Ordered by when a first-time installer needs it, not alphabetically:
      // setup you can't skip, then the things you reach for once it's running,
      // then reference nobody needs to read to get working.
      sidebar: [
        {
          label: 'Setting it up',
          items: [
            { label: 'Start Here', link: '/guides/getting-started/' },
            { label: 'Wiring', link: '/guides/wiring/' },
            { label: 'Config Builder', link: '/config-builder/' },
          ],
        },
        {
          label: 'Once it’s running',
          items: [
            { label: 'Home Assistant', link: '/guides/home-assistant/' },
            { label: 'Troubleshooting', link: '/guides/troubleshooting/' },
            { label: 'Reducing Frame Loss', link: '/guides/uart-optimization/' },
          ],
        },
        {
          label: 'Reference',
          items: [
            { label: 'Configuration Options', link: '/guides/configuration/' },
            { label: 'Web Server & API', link: '/guides/web-server/' },
            { label: 'Saving History to Flash', link: '/guides/tsdb-integration/' },
          ],
        },
      ],
    }),
  ],
});
