# メニューのフォントについて

メニューのフォントは、ビルド時に Google Fonts(https://github.com/google/fonts)から取得し、
メニューで使う文字だけに絞って(サブセット化して)プラグインに埋め込んでいます。

使っているフォント(tools/fonts.txt)はすべて SIL Open Font License 1.1 です。
- Noto Sans JP / Zen Kaku Gothic New / M PLUS Rounded 1c / Zen Maru Gothic / Kiwi Maru
- DotGothic16 / RocknRoll One / Dela Gothic One / Reggae One / Hachi Maru Pop

ライセンス本文: https://openfontlicense.org/open-font-license-official-text/
各フォントの著作権表示は google/fonts リポジトリの各フォルダにある OFL.txt を参照してください。
Switch 本体のフォントは本体の共有フォントをそのまま読み込んでいます(埋め込んでいません)。
