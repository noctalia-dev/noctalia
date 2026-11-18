foreach font_asset : ['nythera-tabler.ttf', 'tabler.json', 'tabler-icons-license.txt', 'NytheraDisplay-Regular.ttf']
  files_assets += [files_asset_copy.copyfile('../../assets/fonts/' + font_asset)]
  install_data('../../assets/fonts/' + font_asset,
    install_dir: get_option('datadir') / 'nythera' / 'files-assets' / 'fonts',
    install_tag: 'files-assets')
endforeach
