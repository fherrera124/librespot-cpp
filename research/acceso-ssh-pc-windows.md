# Acceso SSH a PC Windows remota — registro y reversión

**Fecha:** 2026-09-23
**PC remota:** host `CZC148B0HC` · usuario `arba\francisco.herrera` · Windows 11 23H2 (build 22631) · PowerShell 5.1
**IP (por VPN):** `10.16.150.154`
**PC local:** Linux (ThinkPad T14 Gen 3)

> Objetivo: dar a un agente (Claude Code) un canal de consola no interactivo para ejecutar
> scripts en la PC Windows, mediante OpenSSH Server + autenticación por clave.
> RDP no se tocó y sigue funcionando igual que antes.

---

## 1. Qué se hizo

### En la PC local (Linux)
- Se generó un par de claves SSH dedicado **solo** para este acceso:
  - Privada: `~/.ssh/win_claude` (secreta, NO compartir)
  - Pública: `~/.ssh/win_claude.pub`
- Clave pública (esto es lo que quedó autorizado en Windows):
  ```
  ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBvWELrQ087Yc8+EtqskRHio28OalwdtbXJnId5XrejK claude-code@user-ThinkPad-T14-Gen-3
  ```

### En la PC Windows (ejecutado en PowerShell como Administrador)
1. Instalación del servidor OpenSSH:
   ```powershell
   Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
   ```
2. Arranque del servicio y arranque automático con el sistema:
   ```powershell
   Start-Service sshd
   Set-Service -Name sshd -StartupType Automatic
   ```
3. Regla de firewall para permitir el puerto 22 entrante:
   ```powershell
   New-NetFirewallRule -Name sshd -DisplayName 'OpenSSH Server (sshd)' `
     -Enabled True -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22
   ```
4. Autorización de la clave pública en el archivo de claves para administradores:
   ```powershell
   $key = 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBvWELrQ087Yc8+EtqskRHio28OalwdtbXJnId5XrejK claude-code@user-ThinkPad-T14-Gen-3'
   $f = "$env:ProgramData\ssh\administrators_authorized_keys"
   Add-Content -Path $f -Value $key -Encoding ascii
   ```
5. Permisos ACL estrictos que OpenSSH exige en ese archivo (con SID, válido en Windows en español):
   ```powershell
   icacls $f /inheritance:r /grant "*S-1-5-32-544:F" /grant "*S-1-5-18:F"
   ```
   - `*S-1-5-32-544` = grupo Administradores · `*S-1-5-18` = SISTEMA

---

## 2. Cómo accede un agente

**Requisito previo:** la VPN debe estar conectada (la IP `10.16.150.154` se enruta por el túnel).

### Si el agente corre en ESTA PC local (ya tiene la clave)
```bash
# Comprobar que el puerto 22 responde (el ping no sirve: Windows bloquea ICMP)
timeout 4 bash -c "echo > /dev/tcp/10.16.150.154/22" && echo "22 ABIERTO"

# Conexión / comando suelto
ssh -i ~/.ssh/win_claude francisco.herrera@10.16.150.154 "whoami & hostname"

# Ejecutar PowerShell
ssh -i ~/.ssh/win_claude francisco.herrera@10.16.150.154 \
  "powershell -NoProfile -Command \"Get-Process | Select-Object -First 5\""

# Copiar un script y ejecutarlo
scp -i ~/.ssh/win_claude ./mi_script.ps1 francisco.herrera@10.16.150.154:C:/Users/francisco.herrera/
ssh -i ~/.ssh/win_claude francisco.herrera@10.16.150.154 \
  "powershell -NoProfile -ExecutionPolicy Bypass -File C:/Users/francisco.herrera/mi_script.ps1"
```

### Si el agente corre en OTRA máquina (no tiene esta clave)
1. Genera un par de claves propio en esa máquina:
   ```bash
   ssh-keygen -t ed25519 -f ~/.ssh/win_claude -N ""
   ```
2. Autoriza SU clave pública (`~/.ssh/win_claude.pub`) en Windows repitiendo los pasos 4 y 5
   de la sección 1 con el nuevo texto de clave.
3. Conéctate igual que arriba, con la VPN conectada.

> La clave privada da acceso completo a la sesión del usuario en Windows. Guárdala protegida
> (permisos 600) y no la copies a sitios no confiables.

---

## 3. Cómo revertir todo (dejar la PC como estaba)

Ejecutar en la PC Windows, en **PowerShell como Administrador**, en este orden:

```powershell
# 1. Quitar SOLO nuestra clave del archivo de autorizadas (conserva otras si las hubiera)
$f = "$env:ProgramData\ssh\administrators_authorized_keys"
if (Test-Path $f) {
  (Get-Content $f) | Where-Object { $_ -notmatch 'claude-code@user-ThinkPad' } | Set-Content $f -Encoding ascii
}

# 2. Detener y deshabilitar el servicio SSH
Stop-Service sshd -ErrorAction SilentlyContinue
Set-Service -Name sshd -StartupType Disabled -ErrorAction SilentlyContinue

# 3. Eliminar la regla de firewall del puerto 22
Remove-NetFirewallRule -Name sshd -ErrorAction SilentlyContinue

# 4. Desinstalar el servidor OpenSSH (toma el nombre exacto instalado)
Get-WindowsCapability -Online -Name OpenSSH.Server* | Remove-WindowsCapability -Online

# 5. (Opcional) Borrar la carpeta de configuración de SSH para no dejar rastro.
#    OJO: elimina las host keys y el archivo de claves autorizadas.
Remove-Item "$env:ProgramData\ssh" -Recurse -Force -ErrorAction SilentlyContinue
```

Tras esto, Windows queda como antes: sin servidor SSH, sin regla de firewall y sin la clave
autorizada. RDP no se ve afectado.

### En la PC local (opcional, si ya no vas a usar el acceso)
```bash
rm -f ~/.ssh/win_claude ~/.ssh/win_claude.pub
# y quitar la línea de 10.16.150.154 de ~/.ssh/known_hosts si molesta:
ssh-keygen -R 10.16.150.154
```

---

## 4. Notas
- El acceso **depende de la VPN**. Sin túnel activo, la IP no es alcanzable.
- El `ping` a la máquina falla siempre: el firewall de Windows bloquea ICMP. Para comprobar
  alcance, prueba el puerto TCP (445 casi siempre abierto, o 22 tras el setup).
- Mientras el servicio SSH esté activo y en arranque automático, el acceso persiste entre
  reinicios del PC. Revertir con la sección 3 cuando ya no se necesite.
