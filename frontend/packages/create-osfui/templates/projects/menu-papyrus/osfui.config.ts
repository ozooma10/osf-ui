import { defineConfig } from '@osfui/cli';

export default defineConfig({
  modId: '__OSFUI_MOD_ID_SQ__',
  views: [{
    id: '__OSFUI_VIEW_ID__',
    title: '__OSFUI_VIEW_TITLE__',
    description: 'Generated menu starter for __OSFUI_MOD_ID_SQ__',
    kind: 'menu',
    width: 1200,
    height: 720,
    targetVersion: '__OSFUI_RELEASE_VERSION__',
    pausesGame: false,
  }],
});
