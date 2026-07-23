// @ts-check
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

export default defineConfig({
  site: 'https://rar.github.io',
  base: '/esphome-tigomonitor',
  integrations: [
    starlight({
      title: 'Tigo Monitor',
      // Starlight >=0.30 uses the array form. If the installed version errors
      // on this, use the object form: social: { github: '<url>' }
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
