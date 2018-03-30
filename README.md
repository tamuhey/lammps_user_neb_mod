neb, fix nebの修正

*使い方
  lammps scriptで"neb"を"neb_mod"と書き換えればよい
  Automatic Update of Spring Constantを使用する場合，nebのKspringを数値ではなく"auto"にする  
  Variable Spring Constantを使用する場合，nebのKspringを"Variable Kmax Kd"とする．ex. variable 10.0 8.0

*修正点
1. Ju LiのAutomatic Update of Spring Constantに対応
  便利だが怪しい手法なので本計算として使うべきではない．別資料参照

2. 実装ミス修正
  fix nebでestyle = last/efirst/middleを使用した際，middleにかかる力がparallel = idealでもneighの設定となっている部分を修正
    結果はほとんど変わらないと思うが気持ち悪かったので修正

3. string法のバネ力の修正
  string法(parallel idealで指定)において，元の実装のバネ力がおかしいと思われるので以下のように修正
    前：-k(r-r_ideal)/(2*meandist)
    後：-k(r-r_ideal)
  元の実装では単位があっていないので誤りだと思われる．
  修正後の式にすれば，例えばreplicaが3つのときneighと一致する．

4. thermo出力の有効桁数を上げた
