use serde::{Deserialize, Serialize};
use std::fs;
use std::io::Write;
use std::path::Path;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PrefEntry {
    pub key: String,
    pub value: String,
    pub comment: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PrefsFile {
    pub entries: Vec<PrefEntry>,
    trailing_comments: Vec<String>,
}

impl PrefsFile {
    pub fn from_entries(entries: Vec<PrefEntry>) -> Self {
        PrefsFile {
            entries,
            trailing_comments: Vec::new(),
        }
    }

    pub fn parse(content: &str) -> Self {
        let mut entries = Vec::new();
        let mut pending_comment: Option<String> = None;
        let mut trailing_comments = Vec::new();

        for line in content.lines() {
            let trimmed = line.trim();

            if trimmed.is_empty() {
                if let Some(ref mut c) = pending_comment {
                    c.push('\n');
                } else {
                    pending_comment = Some(String::new());
                }
                continue;
            }

            if trimmed.starts_with('#') || trimmed.starts_with(';') {
                if let Some(ref mut c) = pending_comment {
                    c.push('\n');
                    c.push_str(trimmed);
                } else {
                    pending_comment = Some(trimmed.to_string());
                }
                continue;
            }

            let (key, value) = if let Some(pos) = trimmed.find(|c: char| c.is_whitespace()) {
                (trimmed[..pos].to_string(), trimmed[pos..].trim().to_string())
            } else {
                (trimmed.to_string(), String::new())
            };

            entries.push(PrefEntry {
                key,
                value,
                comment: pending_comment.take(),
            });
        }

        if let Some(c) = pending_comment {
            trailing_comments.push(c);
        }

        PrefsFile {
            entries,
            trailing_comments,
        }
    }

    pub fn serialize(&self) -> String {
        let mut out = String::new();

        for entry in &self.entries {
            if let Some(ref comment) = entry.comment {
                if !comment.is_empty() {
                    out.push_str(comment);
                    out.push('\n');
                } else {
                    out.push('\n');
                }
            }
            out.push_str(&entry.key);
            if !entry.value.is_empty() {
                out.push(' ');
                out.push_str(&entry.value);
            }
            out.push('\n');
        }

        for comment in &self.trailing_comments {
            out.push_str(comment);
            out.push('\n');
        }

        out
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.entries
            .iter()
            .find(|e| e.key == key)
            .map(|e| e.value.as_str())
    }

    pub fn get_all(&self, key: &str) -> Vec<&str> {
        self.entries
            .iter()
            .filter(|e| e.key == key)
            .map(|e| e.value.as_str())
            .collect()
    }

    pub fn set(&mut self, key: &str, value: &str) {
        if let Some(entry) = self.entries.iter_mut().find(|e| e.key == key) {
            entry.value = value.to_string();
        } else {
            self.entries.push(PrefEntry {
                key: key.to_string(),
                value: value.to_string(),
                comment: None,
            });
        }
    }

    pub fn get_int(&self, key: &str) -> Option<i64> {
        let val = self.get(key)?;
        parse_size_value(val)
    }

    pub fn add(&mut self, key: &str, value: &str) {
        self.entries.push(PrefEntry {
            key: key.to_string(),
            value: value.to_string(),
            comment: None,
        });
    }

    #[allow(dead_code)] // needed for multi-value pref editing (e.g. replacing all disk entries); tested, not yet wired to a command
    pub fn remove_all(&mut self, key: &str) {
        self.entries.retain(|e| e.key != key);
    }

    #[allow(dead_code)] // used in tests; will be needed when settings UI reads bool prefs directly
    pub fn get_bool(&self, key: &str) -> Option<bool> {
        let val = self.get(key)?;
        match val.to_lowercase().as_str() {
            "true" | "1" | "yes" => Some(true),
            "false" | "0" | "no" => Some(false),
            _ => None,
        }
    }
}

fn parse_size_value(s: &str) -> Option<i64> {
    let s = s.trim();
    if s.is_empty() {
        return None;
    }

    let (num_str, multiplier) = match s.chars().last() {
        Some('K') | Some('k') => (&s[..s.len() - 1], 1024i64),
        Some('M') | Some('m') => (&s[..s.len() - 1], 1024 * 1024),
        Some('G') | Some('g') => (&s[..s.len() - 1], 1024 * 1024 * 1024),
        _ => (s, 1i64),
    };

    num_str.trim().parse::<i64>().ok().map(|n| n * multiplier)
}

pub fn load_prefs(path: &Path) -> Result<PrefsFile, String> {
    let content = fs::read_to_string(path).map_err(|e| format!("Cannot read prefs: {}", e))?;
    Ok(PrefsFile::parse(&content))
}

pub fn save_prefs(path: &Path, prefs: &PrefsFile) -> Result<(), String> {
    let content = prefs.serialize();
    let mut f = fs::File::create(path).map_err(|e| format!("Cannot write prefs: {}", e))?;
    f.write_all(content.as_bytes())
        .map_err(|e| format!("Write failed: {}", e))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_round_trip() {
        let input = "# ROM and disk\nrom /path/to/rom\nramsize 256M\n\n# Hardware\nscreen win/800/600\nnosound true\n";
        let prefs = PrefsFile::parse(input);
        assert_eq!(prefs.get("rom"), Some("/path/to/rom"));
        assert_eq!(prefs.get("ramsize"), Some("256M"));
        assert_eq!(prefs.get_int("ramsize"), Some(256 * 1024 * 1024));
        assert_eq!(prefs.get_bool("nosound"), Some(true));

        let output = prefs.serialize();
        assert!(output.contains("# ROM and disk"));
        assert!(output.contains("rom /path/to/rom"));
        assert!(output.contains("ramsize 256M"));
    }

    #[test]
    fn test_multi_value() {
        let input = "disk /path/one.dsk\ndisk /path/two.dsk\ncdrom /path/cd.iso\n";
        let prefs = PrefsFile::parse(input);
        let disks = prefs.get_all("disk");
        assert_eq!(disks.len(), 2);
        assert_eq!(disks[0], "/path/one.dsk");
        assert_eq!(disks[1], "/path/two.dsk");
    }

    #[test]
    fn test_size_parsing() {
        assert_eq!(parse_size_value("256M"), Some(268435456));
        assert_eq!(parse_size_value("1G"), Some(1073741824));
        assert_eq!(parse_size_value("512K"), Some(524288));
        assert_eq!(parse_size_value("1024"), Some(1024));
    }

    #[test]
    fn test_set_existing_key() {
        let input = "rom /old/path\nramsize 128M\n";
        let mut prefs = PrefsFile::parse(input);
        prefs.set("rom", "/new/path");
        assert_eq!(prefs.get("rom"), Some("/new/path"));
        let output = prefs.serialize();
        assert!(output.contains("rom /new/path"));
        assert!(!output.contains("/old/path"));
    }

    #[test]
    fn test_set_new_key() {
        let input = "rom /path\n";
        let mut prefs = PrefsFile::parse(input);
        prefs.set("nosound", "true");
        assert_eq!(prefs.get("nosound"), Some("true"));
        let output = prefs.serialize();
        assert!(output.contains("nosound true"));
    }

    #[test]
    fn test_bool_parsing() {
        let input = "nosound true\nnocdrom false\nignoresegv 1\nignoreillegal 0\n";
        let prefs = PrefsFile::parse(input);
        assert_eq!(prefs.get_bool("nosound"), Some(true));
        assert_eq!(prefs.get_bool("nocdrom"), Some(false));
        assert_eq!(prefs.get_bool("ignoresegv"), Some(true));
        assert_eq!(prefs.get_bool("ignoreillegal"), Some(false));
    }

    #[test]
    fn test_comment_preservation() {
        let input = "# Boot config\nrom /path/to/rom\n\n# Hardware\nramsize 256M\n";
        let prefs = PrefsFile::parse(input);
        let output = prefs.serialize();
        assert!(output.contains("# Boot config"));
        assert!(output.contains("# Hardware"));
    }

    #[test]
    fn test_from_entries() {
        let entries = vec![
            PrefEntry { key: "rom".to_string(), value: "/test/rom".to_string(), comment: None },
            PrefEntry { key: "ramsize".to_string(), value: "512M".to_string(), comment: Some("# Memory".to_string()) },
        ];
        let pf = PrefsFile::from_entries(entries);
        assert_eq!(pf.get("rom"), Some("/test/rom"));
        assert_eq!(pf.get_int("ramsize"), Some(512 * 1024 * 1024));
    }

    #[test]
    fn test_empty_input() {
        let prefs = PrefsFile::parse("");
        assert!(prefs.entries.is_empty());
        assert_eq!(prefs.serialize(), "");
    }

    #[test]
    fn test_sheepshaver_prefs_format() {
        let input = r#"# ROM and disk
rom /Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom
disk /Users/Shared/macemu/macos86_fresh.dsk
extfs false

# Hardware
ramsize 256M
jitcachesize 256M
screen win/800/600
nosound true

# Boot
bootdriver 0
nocdrom true
"#;
        let prefs = PrefsFile::parse(input);
        assert_eq!(prefs.get("rom"), Some("/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom"));
        assert_eq!(prefs.get("disk"), Some("/Users/Shared/macemu/macos86_fresh.dsk"));
        assert_eq!(prefs.get("extfs"), Some("false"));
        assert_eq!(prefs.get_int("ramsize"), Some(268435456));
        assert_eq!(prefs.get_int("jitcachesize"), Some(268435456));
        assert_eq!(prefs.get("screen"), Some("win/800/600"));
        assert_eq!(prefs.get_bool("nosound"), Some(true));
        assert_eq!(prefs.get_int("bootdriver"), Some(0));
        assert_eq!(prefs.get_bool("nocdrom"), Some(true));

        let output = prefs.serialize();
        assert!(output.contains("# ROM and disk"));
        assert!(output.contains("# Hardware"));
        assert!(output.contains("# Boot"));
    }
}
