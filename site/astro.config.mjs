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
      // Build-time gate: fails the build on broken internal links + anchors,
      // so relative-link/base-path regressions (see the 2026-07-23 review) can't
      // silently ship again.
      plugins: [starlightLinksValidator()],
      social: [
        { icon: 'github', label: 'GitHub', href: 'https://github.com/RAR/esphome-tigomonitor' },
      ],
      sidebar: [
        { label: 'Config Builder', link: '/config-builder/' },
        {
          label: 'Guides',
          items: [
            { label: 'Configuration', link: '/guides/configuration/' },
            { label: 'Wiring', link: '/guides/wiring/' },
            { label: 'Reducing Frame Loss', link: '/guides/uart-optimization/' },
            { label: 'Web Server & API', link: '/guides/web-server/' },
            { label: 'TSDB Integration', link: '/guides/tsdb-integration/' },
            { label: 'Home Assistant', link: '/guides/home-assistant/' },
            { label: 'Troubleshooting', link: '/guides/troubleshooting/' },
          ],
        },
      ],
    }),
  ],
});
