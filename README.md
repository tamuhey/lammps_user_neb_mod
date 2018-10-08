# [LAMMPS] neb, fix neb の修正

## 使い方

- lammps script で"neb"を"neb_mod"と書き換えればよい
- Automatic Update of Spring Constant を使用する場合，neb の Kspring を数値ではなく"auto"にする  
- Variable Spring Constant を使用する場合，neb の Kspring を"Variable Kmax Kd"とする．ex. variable 10.0 8.0

## 修正点

- Ju Li の Automatic Update of Spring Constant に対応
  - 便利だが怪しい手法なので本計算として使うべきではない．別資料参照

- 実装ミス修正
  - fix neb で estyle = last/efirst/middle を使用した際，middle にかかる力が parallel = ideal でも neigh の設定となっている部分を修正
  - 結果はほとんど変わらないと思うが気持ち悪かったので修正

- string 法のバネ力の修正
  - string 法(parallel ideal で指定)において，元の実装のバネ力がおかしいと思われるので以下のように修正
    - 前：-k(r-r_ideal)/(2\*meandist)
    - 後：-k(r-r_ideal)

- thermo 出力の表示桁数を上げた