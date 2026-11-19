<#
    instalar.ps1 - OrbisInstrumentalizer
    Instala o plugin num PS4 com GoldHEN pela rede, usando o servidor FTP do GoldHEN:
      1. envia o OrbisInstrumentalizer.prx para /data/GoldHEN/plugins/
      2. adiciona o Rock Band 4 ao /data/GoldHEN/plugins.ini (mantendo o resto e salvando um backup)
      3. avisa se o Rock Band 4 instalado não está na versão 02.21

    Uso: .\instalar.ps1 [-IP 192.168.0.10] [-Porta 2121]
#>
param(
    [string]$IP,
    [int]$Porta = 2121
)

$ErrorActionPreference = 'Stop'

$PluginNome   = 'OrbisInstrumentalizer.prx'
$PluginRemoto = "/data/GoldHEN/plugins/$PluginNome"
$IniRemoto    = '/data/GoldHEN/plugins.ini'
$Jogos        = [ordered]@{ 'CUSA02084' = 'Rock Band 4 (EUA)'; 'CUSA02901' = 'Rock Band 4 (Europa)' }
$VersaoCompativel = '02.21'

function Write-Passo([string]$texto) { Write-Host "`n> $texto" -ForegroundColor Cyan }
function Write-Ok([string]$texto) { Write-Host "  OK: $texto" -ForegroundColor Green }
function Write-Aviso([string]$texto) { Write-Host "  AVISO: $texto" -ForegroundColor Yellow }
function Stop-ComErro([string]$texto) { Write-Host "`nERRO: $texto" -ForegroundColor Red; exit 1 }

# curl.exe vem com o Windows 10/11 e fala FTP; o GoldHEN não aceita EPSV
function Invoke-Ftp([string[]]$argumentos) {
    # no PowerShell 5.1 o stderr do curl vira erro e derrubaria o script; o código de saída basta
    $ErrorActionPreference = 'Continue'
    # o FTP do GoldHEN falha de vez em quando, então tenta algumas vezes antes de desistir
    for ($tentativa = 1; $tentativa -le 3; $tentativa++) {
        $saida = & curl.exe --silent --show-error --disable-epsv --connect-timeout 5 --max-time 60 @argumentos 2>&1 |
            ForEach-Object { "$_" }
        $codigo = $LASTEXITCODE
        if ($codigo -eq 0) { break }
        Start-Sleep -Milliseconds 700
    }
    return @{ Codigo = $codigo; Saida = ($saida -join ' ').Trim() }
}

# Lê o APP_VER de um param.sfo
function Get-VersaoSfo([byte[]]$sfo) {
    if ($sfo.Length -lt 20 -or [Text.Encoding]::ASCII.GetString($sfo, 1, 3) -ne 'PSF') { return $null }
    $tabelaChaves = [BitConverter]::ToUInt32($sfo, 0x08)
    $tabelaDados  = [BitConverter]::ToUInt32($sfo, 0x0C)
    $entradas     = [BitConverter]::ToUInt32($sfo, 0x10)
    for ($i = 0; $i -lt $entradas; $i++) {
        $entrada = 0x14 + $i * 16
        $inicioChave = $tabelaChaves + [BitConverter]::ToUInt16($sfo, $entrada)
        $fimChave = [Array]::IndexOf($sfo, [byte]0, [int]$inicioChave)
        $chave = [Text.Encoding]::ASCII.GetString($sfo, $inicioChave, $fimChave - $inicioChave)
        if ($chave -eq 'APP_VER') {
            $tamanho = [BitConverter]::ToUInt32($sfo, $entrada + 4)
            $inicioDado = $tabelaDados + [BitConverter]::ToUInt32($sfo, $entrada + 12)
            return [Text.Encoding]::ASCII.GetString($sfo, $inicioDado, $tamanho).TrimEnd([char]0)
        }
    }
    return $null
}

# Garante que cada jogo tenha uma seção apontando para o plugin, sem mexer no resto
function Add-PluginAoIni([string]$conteudo) {
    $quebra = if ($conteudo -match "`r`n") { "`r`n" } else { "`n" }
    $linhas = New-Object System.Collections.Generic.List[string]
    if ($conteudo.Length -gt 0) { $linhas.AddRange([string[]]($conteudo -split "`r?`n")) }
    while ($linhas.Count -gt 0 -and $linhas[$linhas.Count - 1].Trim() -eq '') { $linhas.RemoveAt($linhas.Count - 1) }

    foreach ($jogo in $Jogos.Keys) {
        $cabecalho = -1
        for ($i = 0; $i -lt $linhas.Count; $i++) {
            if ($linhas[$i].Trim() -ieq "[$jogo]") { $cabecalho = $i; break }
        }
        if ($cabecalho -lt 0) {
            if ($linhas.Count -gt 0) { $linhas.Add('') }
            $linhas.Add("; $($Jogos[$jogo])")
            $linhas.Add("[$jogo]")
            $linhas.Add($PluginRemoto)
            continue
        }
        $jaTem = $false
        for ($i = $cabecalho + 1; $i -lt $linhas.Count -and -not $linhas[$i].Trim().StartsWith('['); $i++) {
            if ($linhas[$i].Trim() -eq $PluginRemoto) { $jaTem = $true; break }
        }
        if (-not $jaTem) { $linhas.Insert($cabecalho + 1, $PluginRemoto) }
    }
    return ($linhas -join $quebra) + $quebra
}

Write-Host '=== Instalador do OrbisInstrumentalizer ===' -ForegroundColor White

# --- encontra o plugin -------------------------------------------------------
$pasta = $PSScriptRoot
$plugin = @((Join-Path $pasta $PluginNome), (Join-Path $pasta "bin\$PluginNome")) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $plugin) { Stop-ComErro "não achei o $PluginNome nesta pasta nem em bin\. Compile o plugin primeiro." }
if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue)) { Stop-ComErro 'curl.exe não encontrado (precisa do Windows 10 ou mais novo).' }

# --- IP do PS4 (lembra o último usado) ---------------------------------------
$arquivoIp = Join-Path $pasta '.ps4-ip'
if (-not $IP) {
    $ultimo = if (Test-Path $arquivoIp) { (Get-Content $arquivoIp -Raw).Trim() } else { '' }
    $pergunta = if ($ultimo) { "IP do PS4 [$ultimo]" } else { 'IP do PS4 (Configurações > Rede > Ver status da conexão)' }
    $IP = (Read-Host $pergunta).Trim()
    if (-not $IP) { $IP = $ultimo }
}
if (-not $IP) { Stop-ComErro 'nenhum IP informado.' }
$base = "ftp://${IP}:$Porta"

# --- conexão -----------------------------------------------------------------
Write-Passo "Conectando ao PS4 em ${IP}:$Porta..."
$r = Invoke-Ftp @('--list-only', "$base/data/")
if ($r.Codigo -ne 0) {
    Stop-ComErro "não consegui conectar. Confira se o PS4 está ligado com o GoldHEN ativo e o servidor FTP habilitado (menu do GoldHEN > Server Settings > FTP), e se o IP está certo.`n  Detalhe: $($r.Saida)"
}
Set-Content -Path $arquivoIp -Value $IP -Encoding ASCII
Write-Ok 'conectado'

# --- versão do Rock Band 4 ----------------------------------------------------
Write-Passo 'Verificando a versão do Rock Band 4...'
$temp = Join-Path ([IO.Path]::GetTempPath()) "orbisinstrumentalizer-$PID"
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    $achouJogo = $false
    foreach ($jogo in $Jogos.Keys) {
        $sfo = Join-Path $temp "$jogo.sfo"
        $r = Invoke-Ftp @("$base/system_data/priv/appmeta/$jogo/param.sfo", '-o', $sfo)
        if ($r.Codigo -ne 0 -or -not (Test-Path $sfo)) { continue }
        $achouJogo = $true
        $versao = Get-VersaoSfo ([IO.File]::ReadAllBytes($sfo))
        if ($versao -eq $VersaoCompativel) {
            Write-Ok "$($Jogos[$jogo]) na versão $versao"
        } elseif ($versao) {
            Write-Aviso "$($Jogos[$jogo]) está na versão $versao. O plugin só funciona na $VersaoCompativel (no jogo aparece 2.3.7)."
        } else {
            Write-Aviso "não consegui ler a versão do $($Jogos[$jogo])."
        }
    }
    if (-not $achouJogo) { Write-Aviso 'não encontrei o Rock Band 4 instalado. O plugin será instalado mesmo assim.' }

    # --- envia o plugin --------------------------------------------------------
    Write-Passo 'Enviando o plugin...'
    $r = Invoke-Ftp @('--ftp-create-dirs', '-T', $plugin, "$base$PluginRemoto")
    if ($r.Codigo -ne 0) { Stop-ComErro "falha ao enviar o plugin.`n  Detalhe: $($r.Saida)" }
    Write-Ok $PluginRemoto

    # --- atualiza o plugins.ini --------------------------------------------------
    Write-Passo 'Atualizando o plugins.ini...'
    $iniLocal = Join-Path $temp 'plugins.ini'
    $r = Invoke-Ftp @("$base$IniRemoto", '-o', $iniLocal)
    $existia = ($r.Codigo -eq 0 -and (Test-Path $iniLocal))
    $original = if ($existia) { [IO.File]::ReadAllText($iniLocal) } else { '' }
    $novo = Add-PluginAoIni $original

    if ($existia -and $novo -eq $original) {
        Write-Ok 'já estava configurado'
    } else {
        if ($existia) {
            $r = Invoke-Ftp @('-T', $iniLocal, "$base$IniRemoto.bak")
            if ($r.Codigo -ne 0) { Stop-ComErro "falha ao salvar o backup do plugins.ini.`n  Detalhe: $($r.Saida)" }
            Write-Ok "backup salvo em $IniRemoto.bak"
        }
        # sem BOM, para o GoldHEN ler a primeira linha direito
        [IO.File]::WriteAllText($iniLocal, $novo, (New-Object System.Text.UTF8Encoding($false)))
        $r = Invoke-Ftp @('--ftp-create-dirs', '-T', $iniLocal, "$base$IniRemoto")
        if ($r.Codigo -ne 0) { Stop-ComErro "falha ao enviar o plugins.ini.`n  Detalhe: $($r.Saida)" }
        Write-Ok ($(if ($existia) { 'Rock Band 4 adicionado' } else { 'criado' }))
    }
} finally {
    Remove-Item -Recurse -Force -Path $temp -ErrorAction SilentlyContinue
}

Write-Host "`nPronto! Ligue os instrumentos, espere parearem e abra o Rock Band 4." -ForegroundColor Green
Write-Host 'Se o jogo já estava aberto, feche e abra de novo para carregar o plugin.'
