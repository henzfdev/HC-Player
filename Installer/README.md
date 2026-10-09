# HC Player 1.5.1 — Installer / Inno Setup

Este diretório preserva a arquitetura SAFE do instalador do HC Player e o AppId permanente de upgrade:

`{805B14D6-30AC-45A2-BE42-CE0EB9F68408}`

## Gerar o Setup

1. No Visual Studio, selecione **Release | x64**.
2. Use **Compilar > Limpar Solução** e **Compilar > Recompilar Solução**.
3. Abra PowerShell **dentro desta pasta `Installer`**.
4. Execute exatamente:

```powershell
powershell -ExecutionPolicy Bypass -File .\GERAR-INSTALADOR-V1.ps1
```

O script localiza a saída Release, baixa/valida os pré-requisitos oficiais Microsoft quando necessário, monta o Payload, executa o checker SAFE e chama o Inno Setup 7.

Saída final:

- `Installer\Output\HC_Player_1.5.1_x64_Setup.exe`
- `Installer\Output\HC_Player_1.5.1_x64_Setup.exe.sha256.txt`

## Proteções do fluxo

- aceita somente `HC Player.exe` x64 com versão `1.5.1.0`;
- rejeita `portable.flag` e sinais de output Portable/self-contained;
- rejeita PDB/LIB/EXP/OBJ/ILK/IOBJ/IPDB;
- valida hashes congelados de libmpv, MediaInfo, ícone do app e ícone do Setup;
- valida assinatura Microsoft dos pré-requisitos;
- mantém `ChangesAssociations=no` e não cria seção `[Registry]` genérica;
- preserva o handoff de idioma com `runasoriginaluser`;
- mantém limpeza de desinstalação seletiva/fail-closed e não altera `UserChoice`;
- não remove runtimes compartilhados.

Após gerar, faça teste real de instalação limpa e upgrade 1.4 → 1.5.1 antes de publicar.
