#!/usr/bin/env python3
"""
Auto-commit script for PlatformIO builds
Analyzes git changes, generates commit message, asks for approval, then commits & pushes
"""

Import("env")
import subprocess
import sys
import os
from pathlib import Path

def run_command(cmd, cwd=None):
    """Run shell command and return output"""
    try:
        result = subprocess.run(
            cmd,
            shell=True,
            cwd=cwd,
            capture_output=True,
            text=True,
            encoding='utf-8'
        )
        return result.stdout.strip(), result.returncode
    except Exception as e:
        return str(e), 1

def analyze_diff_content(diff_text):
    """Analyze actual diff content for semantic changes"""
    keywords = []
    
    # Common patterns in embedded code
    patterns = {
        'servo': ['servo', 'flapper', 'kick', 'attach', 'detach'],
        'motor': ['motor', 'pwm', 'speed', 'direction'],
        'wifi': ['wifi', 'websocket', 'http', 'telnet'],
        'touch': ['touch', 'pointer', 'click', 'button'],
        'delay': ['delay', 'millis', 'timing', 'timeout'],
        'debug': ['debug', 'telnet', 'print', 'log'],
        'control': ['joystick', 'control', 'state', 'command'],
    }
    
    diff_lower = diff_text.lower()
    
    for category, words in patterns.items():
        if any(word in diff_lower for word in words):
            keywords.append(category)
    
    # Check for numeric changes (angles, delays, etc)
    has_numbers = False
    for line in diff_text.split('\n'):
        if line.startswith('+') or line.startswith('-'):
            if any(char.isdigit() for char in line):
                has_numbers = True
                break
    
    return keywords, has_numbers

def generate_commit_message(project_path):
    """Generate intelligent commit message based on git diff"""
    
    # Get list of modified files
    status_output, _ = run_command("git status --short", cwd=project_path)
    
    if not status_output:
        return None  # No changes
    
    lines = status_output.split('\n')
    modified_files = []
    added_files = []
    deleted_files = []
    
    for line in lines:
        if line.strip():
            status = line[:2].strip()
            filename = line[3:].strip()
            
            if 'M' in status:
                modified_files.append(filename)
            elif 'A' in status:
                added_files.append(filename)
            elif 'D' in status:
                deleted_files.append(filename)
            elif '?' in status:
                added_files.append(filename)
    
    # Get actual diff content for semantic analysis
    diff_output, _ = run_command("git diff HEAD", cwd=project_path)
    keywords, has_numbers = analyze_diff_content(diff_output)
    
    # Get diff statistics for line changes
    stat_output, _ = run_command("git diff --stat", cwd=project_path)
    
    # Generate smart message based on what changed
    message_parts = []
    
    # Specific messages based on file + content analysis
    if 'src/main.cpp' in modified_files or 'main.cpp' in modified_files:
        if 'servo' in keywords:
            if 'delay' in keywords and has_numbers:
                message_parts.append("Adjust servo timing and delays")
            elif 'debug' in keywords:
                message_parts.append("Add servo debugging and telnet logging")
            else:
                message_parts.append("Update servo control logic")
        elif 'motor' in keywords:
            message_parts.append("Modify motor control")
        elif 'wifi' in keywords or 'telnet' in keywords:
            message_parts.append("Update network/telnet functionality")
        elif 'debug' in keywords:
            message_parts.append("Add debug logging")
        else:
            message_parts.append("Update main firmware")
    
    if 'data/controller.js' in modified_files or 'controller.js' in modified_files:
        if 'touch' in keywords:
            message_parts.append("Fix touch event handling")
        elif 'servo' in keywords:
            message_parts.append("Update flapper button logic")
        else:
            message_parts.append("Update web controller")
    
    if 'data/index.html' in modified_files or 'index.html' in modified_files:
        if 'touch' in keywords:
            message_parts.append("Improve mobile touch support")
        else:
            message_parts.append("Update web UI")
    
    if 'platformio.ini' in modified_files:
        if 'lib_deps' in diff_output:
            message_parts.append("Add/update dependencies")
        else:
            message_parts.append("Update build configuration")
    
    if added_files:
        file_desc = ', '.join([f.split('/')[-1] for f in added_files[:2]])
        message_parts.append(f"Add {file_desc}")
    
    if deleted_files:
        message_parts.append(f"Remove {len(deleted_files)} file(s)")
    
    # Fallback generic message
    if not message_parts:
        if modified_files:
            file_list = ', '.join([f.split('/')[-1] for f in modified_files[:2]])
            message_parts.append(f"Update {file_list}")
    
    commit_msg = "; ".join(message_parts) if message_parts else "Update project files"
    
    # Add context if specific keywords detected
    if keywords and len(message_parts) == 1:
        if 'servo' in keywords and 'delay' in keywords:
            commit_msg += " (timing adjustments)"
        elif 'touch' in keywords and 'debug' in keywords:
            commit_msg += " (add mobile debugging)"
    
    return commit_msg

def auto_commit_and_push():
    """Main function to handle auto-commit workflow"""
    
    project_path = r"C:\Users\Sanjay Sajeev\OneDrive\Documents\PlatformIO\Projects\web_robo_soccer"
    
    print("\n" + "="*70)
    print("  AUTO-COMMIT TO GITHUB")
    print("="*70)
    
    # Check if there are changes to commit
    status_output, _ = run_command("git status --short", cwd=project_path)
    
    if not status_output:
        print("✓ No changes to commit - proceeding with build...")
        print("="*70 + "\n")
        return
    
    print("\nChanged files:")
    print(status_output)
    print()
    
    # Generate suggested commit message
    suggested_msg = generate_commit_message(project_path)
    
    print(f"Suggested commit message:")
    print(f"  → {suggested_msg}")
    print()
    
    # Ask user for approval/modification
    print("Options:")
    print("  [Enter]  - Use suggested message")
    print("  [Type]   - Enter custom message")
    print("  [skip]   - Skip commit and just build")
    print()
    
    user_input = input("Your choice: ").strip()
    
    if user_input.lower() == 'skip':
        print("\n⊘ Skipping commit - proceeding with build...")
        print("="*70 + "\n")
        return
    
    # Use custom message if provided, otherwise use suggested
    commit_msg = user_input if user_input else suggested_msg
    
    print(f"\n⚙ Committing with message: '{commit_msg}'")
    
    # Git add all changes
    _, ret = run_command("git add -A", cwd=project_path)
    if ret != 0:
        print("✗ Failed to stage files")
        sys.exit(1)
    
    # Git commit
    commit_cmd = f'git commit -m "{commit_msg}"'
    output, ret = run_command(commit_cmd, cwd=project_path)
    if ret != 0:
        print(f"✗ Commit failed: {output}")
        sys.exit(1)
    
    print("✓ Committed successfully")
    
    # Git push
    print("⚙ Pushing to GitHub...")
    output, ret = run_command("git push", cwd=project_path)
    if ret != 0:
        print(f"✗ Push failed: {output}")
        sys.exit(1)
    
    print("✓ Pushed to GitHub successfully")
    print("="*70 + "\n")
    print("▶ Proceeding with build...\n")

# Execute before build
auto_commit_and_push()
