foreach locale : [
  'ar', 'be-Latn', 'be', 'ca', 'cs', 'de', 'en-GB', 'en', 'es', 'fr',
  'gl-ES', 'he', 'hu', 'it', 'ja', 'ko', 'ku', 'nl', 'nn', 'pl',
  'pt-BR', 'ro', 'ru', 'sv', 'tr', 'uk-UA', 'vi', 'zh-Hans', 'zh-Hant',
]
  files_assets += [files_asset_copy.copyfile('../../assets/translations/' + locale + '.json')]
  install_data('../../assets/translations/' + locale + '.json',
    install_dir: get_option('datadir') / 'nythera' / 'files-assets' / 'translations',
    install_tag: 'files-assets')
endforeach
