# Guía de Configuración de VS2022 para Blackmagic DeckLink SDK 15.3 y Spout 2.0.7

Esta guía proporciona instrucciones detalladas paso a paso para configurar las propiedades del proyecto Visual Studio 2022 (Include Directories, Library Directories y Linker Inputs) para integrar el Blackmagic DeckLink SDK 15.3 y Spout 2.0.7.

## Índice

1. [Prerrequisitos](#prerrequisitos)
2. [Descarga e Instalación de SDKs](#descarga-e-instalación-de-sdks)
3. [Configuración de Blackmagic DeckLink SDK 15.3](#configuración-de-blackmagic-decklink-sdk-153)
4. [Configuración de Spout 2.0.7](#configuración-de-spout-207)
5. [Configuración Combinada del Proyecto](#configuración-combinada-del-proyecto)
6. [Verificación de la Configuración](#verificación-de-la-configuración)
7. [Resolución de Problemas](#resolución-de-problemas)

---

## Prerrequisitos

Antes de comenzar, asegúrese de tener instalado:

- ✅ **Visual Studio 2022** (Community, Professional o Enterprise)
  - Workload: "Desktop development with C++"
  - Componente: Windows 10/11 SDK (10.0.19041.0 o superior)
- ✅ **DirectX 11 Runtime**
- ✅ **Controladores Blackmagic Desktop Video** (versión 15.3+)

---

## Descarga e Instalación de SDKs

### Blackmagic DeckLink SDK 15.3

1. **Descarga**:
   - Visite: [https://www.blackmagicdesign.com/developer/product/capture-and-playback](https://www.blackmagicdesign.com/developer/product/capture-and-playback)
   - Inicie sesión con su cuenta de desarrollador (registre una si no tiene)
   - Descargue "**Desktop Video SDK 15.3**" para Windows

2. **Instalación**:
   - Extraiga el archivo descargado
   - Copie la carpeta del SDK a una ubicación permanente:
   ```
   C:\SDK\Blackmagic DeckLink SDK 15.3\
   ```

3. **Estructura del SDK**:
   ```
   C:\SDK\Blackmagic DeckLink SDK 15.3\
   ├── Win/
   │   ├── include/
   │   │   ├── DeckLinkAPI_h.h
   │   │   ├── DeckLinkAPI_i.c
   │   │   ├── DeckLinkAPIVersion.h
   │   │   └── ... (otros headers)
   │   ├── DirectShow/
   │   └── Samples/
   └── Documentation/
   ```

### Spout 2.0.7

1. **Descarga**:
   - Visite: [https://github.com/leadedge/Spout2/releases](https://github.com/leadedge/Spout2/releases)
   - Descargue "**Spout 2.0.7**" (archivo zip o repositorio)

2. **Instalación**:
   - Extraiga a una ubicación permanente:
   ```
   C:\SDK\Spout2-2.0.7\
   ```

3. **Compilar SpoutLibrary** (si se usa la versión precompilada, omitir):
   - Abra `C:\SDK\Spout2-2.0.7\SPOUTSDK\SpoutLibrary\SpoutLibrary.vcxproj` en VS2022
   - Compile en configuración **Release|x64**
   - Los archivos generados estarán en:
     - `SpoutLibrary.lib` (biblioteca estática)
     - `SpoutLibrary.dll` (biblioteca dinámica)

4. **Estructura del SDK**:
   ```
   C:\SDK\Spout2-2.0.7\
   ├── SPOUTSDK/
   │   ├── SpoutDirectX/
   │   │   ├── SpoutDirectX.h
   │   │   ├── SpoutDX.h
   │   │   └── SpoutDX.cpp
   │   ├── SpoutGL/
   │   │   ├── Spout.h
   │   │   └── Spout.cpp
   │   └── SpoutLibrary/
   │       ├── x64/
   │       │   ├── Release/
   │       │   │   ├── SpoutLibrary.lib
   │       │   │   └── SpoutLibrary.dll
   │       └── SpoutLibrary.h
   └── BUILD/
       └── Binaries/x64/
           ├── SpoutLibrary.lib
           └── SpoutLibrary.dll
   ```

---

## Configuración de Blackmagic DeckLink SDK 15.3

### Paso 1: Abrir Propiedades del Proyecto

1. Abra su solución en Visual Studio 2022
2. En el **Explorador de Soluciones**, haga clic derecho en el proyecto **VIB**
3. Seleccione **Properties** (Alt+Enter)

### Paso 2: Configurar Include Directories

> ⚠️ **Importante**: Configure tanto **Debug** como **Release** para x64

1. En el diálogo de propiedades:
   - Configuration: **All Configurations**
   - Platform: **x64**

2. Navegue a: **C/C++** → **General**

3. Localice **Additional Include Directories**

4. Haga clic en el dropdown y seleccione **\<Edit...\>**

5. Agregue la siguiente ruta:
   ```
   C:\SDK\Blackmagic DeckLink SDK 15.3\Win\include
   ```

6. Clic en **OK**

**Resultado Visual**:
```
Additional Include Directories:
$(ProjectDir);C:\SDK\Blackmagic DeckLink SDK 15.3\Win\include;%(AdditionalIncludeDirectories)
```

### Paso 3: Configurar Library Directories

> 📝 **Nota**: El DeckLink SDK 15.3 para Windows NO requiere bibliotecas .lib externas. La API se integra mediante COM (Component Object Model).

El DeckLink SDK utiliza interfaces COM que se registran automáticamente cuando instala el software **Desktop Video**. Por lo tanto:

- ❌ NO necesita agregar Library Directories adicionales
- ❌ NO necesita agregar .lib files al Linker
- ✅ Solo necesita incluir `DeckLinkAPI_i.c` en su proyecto

### Paso 4: Agregar Archivo de Implementación COM

1. En el **Explorador de Soluciones**, haga clic derecho en su proyecto
2. Seleccione **Add** → **Existing Item...**
3. Navegue a: `C:\SDK\Blackmagic DeckLink SDK 15.3\Win\include\`
4. Seleccione `DeckLinkAPI_i.c`
5. Clic en **Add**

### Paso 5: Verificar Dependencias Necesarias para COM

En **Linker** → **Input** → **Additional Dependencies**, asegúrese de que exista:
```
ole32.lib
oleaut32.lib
```

Estos generalmente ya están incluidos por defecto en proyectos de Windows.

### Resumen de Configuración DeckLink SDK 15.3

| Propiedad | Configuración |
|-----------|---------------|
| **Include Directories** | `C:\SDK\Blackmagic DeckLink SDK 15.3\Win\include` |
| **Library Directories** | (No requerido - usa COM) |
| **Linker Inputs** | `ole32.lib; oleaut32.lib` (generalmente ya incluidos) |
| **Archivos a agregar** | `DeckLinkAPI_i.c` (agregar al proyecto) |

---

## Configuración de Spout 2.0.7

### Paso 1: Decidir el Método de Integración

Spout ofrece dos métodos:

| Método | Ventajas | Desventajas |
|--------|----------|-------------|
| **SpoutLibrary** (recomendado) | Fácil integración, DLL único | Requiere distribuir DLL |
| **Archivos fuente** | Sin dependencias DLL | Más archivos en el proyecto |

**Para esta guía usaremos SpoutLibrary** (método recomendado para DirectX 11).

### Paso 2: Configurar Include Directories

1. Abra las propiedades del proyecto (Alt+Enter)
2. Configuration: **All Configurations**, Platform: **x64**
3. Navegue a: **C/C++** → **General** → **Additional Include Directories**
4. Agregue las rutas:
   ```
   C:\SDK\Spout2-2.0.7\SPOUTSDK\SpoutDirectX
   C:\SDK\Spout2-2.0.7\SPOUTSDK\SpoutLibrary
   ```

**Resultado Visual**:
```
Additional Include Directories:
$(ProjectDir);C:\SDK\Blackmagic DeckLink SDK 15.3\Win\include;C:\SDK\Spout2-2.0.7\SPOUTSDK\SpoutDirectX;C:\SDK\Spout2-2.0.7\SPOUTSDK\SpoutLibrary;%(AdditionalIncludeDirectories)
```

### Paso 3: Configurar Library Directories

1. En las propiedades del proyecto
2. Navegue a: **Linker** → **General** → **Additional Library Directories**
3. Haga clic en **\<Edit...\>**
4. Agregue la ruta según su configuración:

   **Para Release**:
   ```
   C:\SDK\Spout2-2.0.7\BUILD\Binaries\x64
   ```
   
   **O si compiló manualmente**:
   ```
   C:\SDK\Spout2-2.0.7\SPOUTSDK\SpoutLibrary\x64\Release
   ```

**Resultado Visual**:
```
Additional Library Directories:
C:\SDK\Spout2-2.0.7\BUILD\Binaries\x64;%(AdditionalLibraryDirectories)
```

### Paso 4: Configurar Linker Inputs

1. Navegue a: **Linker** → **Input** → **Additional Dependencies**
2. Haga clic en **\<Edit...\>**
3. Agregue:
   ```
   SpoutLibrary.lib
   ```

**Resultado Visual**:
```
Additional Dependencies:
d3d11.lib;dxgi.lib;SpoutLibrary.lib;ole32.lib;oleaut32.lib;%(AdditionalDependencies)
```

### Paso 5: Configurar Copia del DLL (Post-Build Event)

1. Navegue a: **Build Events** → **Post-Build Event**
2. En **Command Line**, agregue:
   ```
   xcopy /Y /D "C:\SDK\Spout2-2.0.7\BUILD\Binaries\x64\SpoutLibrary.dll" "$(OutDir)"
   ```

Esto copiará automáticamente el DLL a la carpeta de salida después de cada compilación.

### Resumen de Configuración Spout 2.0.7

| Propiedad | Configuración |
|-----------|---------------|
| **Include Directories** | `C:\SDK\Spout2-2.0.7\SPOUTSDK\SpoutDirectX`<br>`C:\SDK\Spout2-2.0.7\SPOUTSDK\SpoutLibrary` |
| **Library Directories** | `C:\SDK\Spout2-2.0.7\BUILD\Binaries\x64` |
| **Linker Inputs** | `SpoutLibrary.lib` |
| **DLL necesario** | `SpoutLibrary.dll` (copiar a carpeta de salida) |

---

## Configuración Combinada del Proyecto

### Configuración Final Completa

Después de configurar ambos SDKs, sus propiedades deben verse así:

#### C/C++ → General → Additional Include Directories
```
$(ProjectDir);C:\SDK\Blackmagic DeckLink SDK 15.3\Win\include;C:\SDK\Spout2-2.0.7\SPOUTSDK\SpoutDirectX;C:\SDK\Spout2-2.0.7\SPOUTSDK\SpoutLibrary;%(AdditionalIncludeDirectories)
```

#### Linker → General → Additional Library Directories
```
C:\SDK\Spout2-2.0.7\BUILD\Binaries\x64;%(AdditionalLibraryDirectories)
```

#### Linker → Input → Additional Dependencies
```
d3d11.lib
dxgi.lib
d3dcompiler.lib
SpoutLibrary.lib
ole32.lib
oleaut32.lib
%(AdditionalDependencies)
```

#### Build Events → Post-Build Event → Command Line
```
xcopy /Y /D "C:\SDK\Spout2-2.0.7\BUILD\Binaries\x64\SpoutLibrary.dll" "$(OutDir)"
```

### Actualización del Archivo .vcxproj

Para referencia, aquí está cómo debería verse la sección relevante de su archivo `.vcxproj`:

```xml
<ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
  <ClCompile>
    <AdditionalIncludeDirectories>
      $(ProjectDir);
      C:\SDK\Blackmagic DeckLink SDK 15.3\Win\include;
      C:\SDK\Spout2-2.0.7\SPOUTSDK\SpoutDirectX;
      C:\SDK\Spout2-2.0.7\SPOUTSDK\SpoutLibrary;
      %(AdditionalIncludeDirectories)
    </AdditionalIncludeDirectories>
    <LanguageStandard>stdcpp20</LanguageStandard>
  </ClCompile>
  <Link>
    <AdditionalLibraryDirectories>
      C:\SDK\Spout2-2.0.7\BUILD\Binaries\x64;
      %(AdditionalLibraryDirectories)
    </AdditionalLibraryDirectories>
    <AdditionalDependencies>
      d3d11.lib;
      dxgi.lib;
      d3dcompiler.lib;
      SpoutLibrary.lib;
      ole32.lib;
      oleaut32.lib;
      %(AdditionalDependencies)
    </AdditionalDependencies>
  </Link>
  <PostBuildEvent>
    <Command>xcopy /Y /D "C:\SDK\Spout2-2.0.7\BUILD\Binaries\x64\SpoutLibrary.dll" "$(OutDir)"</Command>
  </PostBuildEvent>
</ItemDefinitionGroup>
```

---

## Verificación de la Configuración

### Código de Prueba

Cree un archivo `test_sdks.cpp` para verificar que ambos SDKs están correctamente configurados:

```cpp
// test_sdks.cpp - Verificación de integración de SDKs
#include <iostream>
#include <Windows.h>

// DeckLink SDK
#include "DeckLinkAPI_h.h"

// Spout SDK
#include "SpoutLibrary.h"

int main()
{
    std::cout << "=== Prueba de Integración de SDKs ===" << std::endl;
    
    // Inicializar COM (necesario para DeckLink)
    HRESULT hr = CoInitialize(nullptr);
    if (FAILED(hr)) {
        std::cerr << "Error: No se pudo inicializar COM" << std::endl;
        return 1;
    }
    
    // ========== PRUEBA DECKLINK SDK ==========
    std::cout << "\n[DeckLink SDK 15.3]" << std::endl;
    
    IDeckLinkIterator* deckLinkIterator = nullptr;
    hr = CoCreateInstance(
        CLSID_CDeckLinkIterator,
        nullptr,
        CLSCTX_ALL,
        IID_IDeckLinkIterator,
        (void**)&deckLinkIterator
    );
    
    if (SUCCEEDED(hr) && deckLinkIterator) {
        std::cout << "  ✓ DeckLink Iterator creado exitosamente" << std::endl;
        
        IDeckLink* deckLink = nullptr;
        int deviceCount = 0;
        
        while (deckLinkIterator->Next(&deckLink) == S_OK) {
            deviceCount++;
            
            BSTR deviceName;
            if (deckLink->GetDisplayName(&deviceName) == S_OK) {
                std::wcout << L"  ✓ Dispositivo encontrado: " << deviceName << std::endl;
                SysFreeString(deviceName);
            }
            
            deckLink->Release();
        }
        
        if (deviceCount == 0) {
            std::cout << "  ⚠ No se encontraron dispositivos DeckLink (normal si no hay hardware conectado)" << std::endl;
        }
        
        deckLinkIterator->Release();
    } else {
        std::cerr << "  ✗ Error: No se pudo crear DeckLink Iterator" << std::endl;
        std::cerr << "    Verifique que Desktop Video esté instalado" << std::endl;
    }
    
    // ========== PRUEBA SPOUT SDK ==========
    std::cout << "\n[Spout SDK 2.0.7]" << std::endl;
    
    SPOUTLIBRARY* spout = GetSpout();
    if (spout) {
        std::cout << "  ✓ Spout Library cargada exitosamente" << std::endl;
        
        // Obtener número de senders activos
        int senderCount = spout->GetSenderCount();
        std::cout << "  ✓ Senders Spout activos: " << senderCount << std::endl;
        
        // Listar senders si hay alguno
        if (senderCount > 0) {
            char senderName[256];
            for (int i = 0; i < senderCount; i++) {
                if (spout->GetSender(i, senderName, 256)) {
                    std::cout << "    - " << senderName << std::endl;
                }
            }
        }
        
        spout->Release();
    } else {
        std::cerr << "  ✗ Error: No se pudo cargar Spout Library" << std::endl;
    }
    
    // Limpieza
    CoUninitialize();
    
    std::cout << "\n=== Prueba Completada ===" << std::endl;
    return 0;
}
```

### Pasos para Compilar y Probar

1. Agregue `test_sdks.cpp` a su proyecto
2. **Build** → **Build Solution** (Ctrl+Shift+B)
3. Si compila sin errores, los SDKs están correctamente configurados
4. Ejecute para verificar que los SDKs funcionan en tiempo de ejecución

### Errores Comunes y Soluciones

| Error | Causa | Solución |
|-------|-------|----------|
| `Cannot open include file: 'DeckLinkAPI_h.h'` | Include Directory incorrecto | Verifique la ruta del SDK en Additional Include Directories |
| `Cannot open include file: 'SpoutLibrary.h'` | Include Directory incorrecto | Verifique las rutas de Spout en Additional Include Directories |
| `LNK1104: cannot open file 'SpoutLibrary.lib'` | Library Directory incorrecto | Verifique la ruta en Additional Library Directories |
| `LNK2019: unresolved external symbol` (DeckLink) | Falta DeckLinkAPI_i.c | Agregue el archivo al proyecto |
| `LNK2019: unresolved external symbol` (Spout) | Falta SpoutLibrary.lib | Agregue a Additional Dependencies |
| Runtime: "SpoutLibrary.dll not found" | DLL no copiado | Configure Post-Build Event o copie manualmente |
| Runtime: "Class not registered" (DeckLink) | Desktop Video no instalado | Instale Blackmagic Desktop Video |

---

## Resolución de Problemas

### Problema: IntelliSense no reconoce los headers

**Solución**:
1. Cierre Visual Studio
2. Elimine la carpeta `.vs` en el directorio de la solución
3. Elimine el archivo `*.vcxproj.user`
4. Reabra la solución

### Problema: Error de enlace con múltiples definiciones

**Solución** (para DeckLink):
- Asegúrese de que `DeckLinkAPI_i.c` esté incluido en **solo un** archivo .cpp de su proyecto
- O configure el archivo con: **Properties** → **C/C++** → **Precompiled Headers** → **Not Using Precompiled Headers**

### Problema: Configuración diferente entre Debug y Release

**Solución**:
1. En las propiedades del proyecto, seleccione **Configuration: All Configurations**
2. Aplique los cambios
3. Alternativamente, configure cada modo por separado si necesita rutas diferentes (ej: bibliotecas Debug vs Release de Spout)

### Problema: El proyecto no compila después de actualizar el SDK

**Solución**:
1. **Build** → **Clean Solution**
2. Elimine las carpetas `bin` y `obj`
3. Actualice las rutas si la versión del SDK cambió
4. **Build** → **Rebuild Solution**

---

## Recursos Adicionales

- [Documentación DeckLink SDK](https://www.blackmagicdesign.com/developer/product/capture-and-playback)
- [Repositorio Spout2](https://github.com/leadedge/Spout2)
- [Guía de SpoutDirectX](https://github.com/leadedge/Spout2/tree/master/SPOUTSDK/SpoutDirectX)

---

## Checklist Final

- [ ] Blackmagic Desktop Video instalado
- [ ] DeckLink SDK 15.3 extraído a ubicación permanente
- [ ] Spout 2.0.7 extraído y SpoutLibrary compilado
- [ ] Include Directories configurados para ambos SDKs
- [ ] Library Directories configurados para Spout
- [ ] Additional Dependencies incluye SpoutLibrary.lib
- [ ] DeckLinkAPI_i.c agregado al proyecto
- [ ] Post-Build Event copia SpoutLibrary.dll
- [ ] Proyecto compila sin errores
- [ ] Prueba de verificación ejecuta correctamente

---

*Última actualización: Marzo 2026*
*Compatible con: Visual Studio 2022, DeckLink SDK 15.3, Spout 2.0.7*
