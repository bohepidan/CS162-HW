const SEND_BUF_SIZE: usize = 1024;

use std::env;

use crate::args;

use crate::http;
use crate::http::*;
use crate::stats::*;

use clap::Parser;
use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;
use tokio::net::TcpStream;
use tokio::net::TcpListener;
use tokio::fs::File;

use anyhow::Result;

pub fn main() -> Result<()> {
    // Configure logging
    // You can print logs (to stderr) using
    // `log::info!`, `log::warn!`, `log::error!`, etc.
    env_logger::Builder::new()
        .filter_level(log::LevelFilter::Info)
        .init();

    // Parse command line arguments
    let args = args::Args::parse();

    // Set the current working directory
    env::set_current_dir(&args.files)?;

    // Print some info for debugging
    log::info!("HTTP server initializing ---------");
    log::info!("Port:\t\t{}", args.port);
    log::info!("Num threads:\t{}", args.num_threads);
    log::info!("Directory:\t\t{}", &args.files);
    log::info!("----------------------------------");

    // Initialize a thread pool that starts running `listen`
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .worker_threads(args.num_threads)
        .build()?
        .block_on(listen(args.port))
}

async fn listen(port: u16) -> Result<()> {
    // Hint: you should call `handle_socket` in this function.
    let addr = format!("127.0.0.1:{}", port);
    let listener = TcpListener::bind(addr).await?;
    loop {
        let (socket, _) = listener.accept().await?;
        tokio::spawn(handle_socket(socket));
    }
}

// Handles a single connection via `socket`.
async fn handle_socket(mut socket: TcpStream) -> Result<()> {
    loop{
        //Read request from client
        socket.readable().await?;
        let req = parse_request(&mut socket).await?; 

        //And resopnse.
        match req.method.as_str() {
            "GET" => response_get(&mut socket, req.path).await?,
            _ => continue
        }
    }
}

async fn response_get(socket:&mut TcpStream, _path: String) -> Result<()> {
    let mut path = String::from(_path);
    path.insert(0, '.');

    let attr = match tokio::fs::metadata(&path).await {
        Ok(n) => n,
        Err(e) => match e.kind() {
            tokio::io::ErrorKind::NotFound => {
                //File Not Found
                response_not_found(socket).await?;
                return Ok(());
            }
            _ => {
                log::warn!("Fopen err:{}", e);
                return Ok(());
            }
        }
    };

    if attr.is_dir() {
        let index = format_index(&path);

        match File::open(&index).await {
            Ok(mut f) => {
                //There is an index.html file.
                response_file(socket, &index, &mut f).await?;
            }
            Err(ref e) if e.kind() == tokio::io::ErrorKind::NotFound => {
                //Cant find an index.html file
                let mut content = String::new();
                let mut rd = tokio::fs::read_dir(&path).await?;

                content.push_str("<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"UTF-8\"\n</head>\n<body>\n");

                content.push_str(&format_href(&format!("{}/../", path), ".."));
                content.push_str(&format_href(&path, "."));

                while let Some(entry) = rd.next_entry().await? {
                    content.push_str(&format_href(
                        entry.path().to_str().unwrap(), 
                        entry.file_name().to_str().unwrap()));
                }

                content.push_str("\n</body>\n</html>\n");

                start_response(socket, 200).await?;
                send_header(socket, "Content-Type", "text/html").await?;
                send_header(socket, "Content-Length", &content.len().to_string()).await?;
                end_headers(socket).await?;
                socket.write_all(&content.as_bytes()).await?;
            }
            Err(e) => {
                    log::warn!("Fopen err:{}", e);
                    return Ok(());
            }
        }
    }else if attr.is_file() {
        let mut f = File::open(&path).await?;
        response_file(socket, &path, &mut f).await?;
    }else{
        log::warn!("WTF is attr?{:?}", attr.file_type());
    }

    Ok(())
}

async fn response_file(socket: &mut TcpStream, path: &str, f:&mut File) -> Result<()>{
    start_response(socket, 200).await?;
    send_header(socket, "Content-Type", get_mime_type(path)).await?;
    send_header(socket, "Content-Length", format!("{}", f.metadata().await?.len()).as_str()).await?;
    end_headers(socket).await?;

    send_file(socket, f).await?;
    Ok(())
}

//Send the content of F to socket, where F is assumed to be opened.
async fn send_file(socket: &mut TcpStream, f: &mut File) -> Result<()> {
    let mut buf = [0; SEND_BUF_SIZE];
    loop {
        let mut start;
        let mut n;
        let mut end = 0;

        while{
            start = end;
            n = f.read(&mut buf[start..]).await?;
            end = start + n;
            n > 0
        }{
            if end == SEND_BUF_SIZE {
                break;
            }
        }

        socket.write_all(&buf[..end]).await?;
        if n == 0 {
            break;
        }
    }
    Ok(())
}

async fn response_not_found(socket:&mut TcpStream) -> Result<()> {
    start_response(socket, 404).await?;
    end_headers(socket).await?;
    Ok(())
}
/* 
fn ok_or_warn(res: Result<()>){
    match res {
        Ok(()) => {}
        Err(e) => log::warn!("statr_response err:{}", e)
    }
}
*/

// You are free (and encouraged) to add other funtions to this file.
// You can also create your own modules as you see fit.
