# RetroFE's Command Line Interface (CLI)
RetroFE supports a variety of command-line options to help automate configuration, streamline development, and troubleshoot the frontend.

## 📌 Usage
Run RetroFE with arguments to trigger specific actions:

```bash
retrofe [-option] [value]
````

All settings available in `settings.conf` can also be passed via the command line in `-key value` format.

**CLI options always take highest priority**, overriding values in `settingsX.conf` files.

## 🧰 Available Options
| Option         | Alias                | Description                                                                        |
| -------------- | -------------------- | ---------------------------------------------------------------------------------- |
| `-h`           | `--help`             | Display this help message                                                          |
| `-v`           | `--version`          | Print the current RetroFE version                                                  |
| `-cc`          | `--createcollection` | Create a new collection folder structure <br>Usage: `-cc [collectionName] {local}` |
| `-rdb`         | `--rebuilddatabase`  | Rebuild the metadata database from `/meta`                                         |
| `-su`          | `--showusage`        | List all available global settings and their descriptions                          |
| `-sc`          | `--showconfig`       | Display all active settings loaded from `settingsX.conf`                           |
| `-C`           | `--createconfig`     | Generate a default `settings.conf` and a `README.md`                               |
| `-dump`        | `--dumpproperties`   | Dump current settings to `properties.txt`                                          |
| `-gstdotdebug` | `--gstdotdebug`      | Enable GStreamer DOT graph generation for debugging                                |

## ⚙️ Passing Configuration via CLI
You can pass any configuration setting using key-value pairs:

```bash
retrofe -log DEBUG -vSync true -muteVideo yes
```

## 🧪 Examples

```bash
# Create a new collection directory
retrofe -cc "Super Nintendo"

# Rebuild the metadata database
retrofe -rdb

# Dump all settings to a file
retrofe -dump

# Set logging and mute video via CLI
retrofe -log DEBUG -muteVideo true
```

---

## 🔗 Resources

* 📘 [RetroFE GitHub Repository](https://github.com/CoinOPS-Official/RetroFE/)
* 🌐 [RetroFE Official Website](http://retrofe.nl/)
